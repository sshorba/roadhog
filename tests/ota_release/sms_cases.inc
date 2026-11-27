static void owner_command(sms_control_t *sms, const char *sender, const char *text)
{
    sms_request_t request;
    snprintf(request.sender, sizeof(request.sender), "%s", sender);
    snprintf(request.text, sizeof(request.text), "%s", text);
    assert(ota_sms_service_command(sms, &fixture_modem, &request));
}

/* Deliver an SMS-DELIVER TPDU in the format used by the production UART parser. */
static void pdu_command(sms_control_t *sms, const char *sender, const char *text, uint8_t flags)
{
    size_t digits = strlen(sender + 1);
    uint8_t pdu[160] = {0, flags, (uint8_t)digits, 0x91};
    size_t used = 4;
    for (size_t i = 0; i < digits; i += 2) {
        unsigned high = i + 1 < digits ? (unsigned)(sender[i + 2] - '0') : 15;
        pdu[used++] = (high << 4) | (sender[i + 1] - '0');
    }
    used += 9; // PID, DCS and service-center timestamp
    size_t chars = strlen(text);
    pdu[used++] = chars;
    for (size_t i = 0; i < chars; ++i) {
        size_t bit = i * 7;
        unsigned shifted = (unsigned char)text[i] << (bit % 8);
        pdu[used + bit / 8] |= shifted & 255;
        if (bit % 8 > 1) {
            pdu[used + bit / 8 + 1] |= shifted >> 8;
        }
    }
    used += (chars * 7 + 7) / 8;
    char line[2 * sizeof(pdu) + 1];
    snprintf(line, sizeof(line), "+CMT: ,%zu", used - 1);
    assert(sms_control_notification(sms, line));
    for (size_t i = 0; i < used; ++i) {
        line[i * 2] = hex[pdu[i] >> 4];
        line[i * 2 + 1] = hex[pdu[i] & 15];
    }
    line[2 * used] = 0;
    assert(sms_control_notification(sms, line));
}

static void test_pdu(const modem_config_t *config, sms_control_t *sms)
{
    pdu_command(sms, OWNER, "OTA CHECK", 0x40);
    pdu_command(sms, OTHER_USER, "OTA CHECK", 0);
    sms_control_service(sms, &fixture_modem);
    assert(acks == 2 && sent == 0 && queued == OTA_SMS_NONE);
    pdu_command(sms, OWNER, "ota check", 0);
    fail_ack = true;
    sms_control_service(sms, &fixture_modem);
    assert(acks == 3 && sent == 0 && queued == OTA_SMS_NONE);
    pdu_command(sms, OWNER, "ota check", 0);
    fail_ack = false;
    sms_control_service(sms, &fixture_modem);
    assert(acks == 4 && sent == 1 && queued == OTA_SMS_CHECK && !requests);
    assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
    assert(requests == 1 && challenge.expires && !touched_sectors && !selections);
}

static void test_sms(const modem_config_t *config, sms_control_t *sms)
{
    ota_sms_init();
    sms->owner = OWNER;
    if (is_case("sms-pdu")) {
        test_pdu(config, sms);
        return;
    }
    if (is_case("sms-equal")) {
        running_version = "2";
    } else if (is_case("sms-older")) {
        running_version = "3";
    }
    owner_command(sms, OTHER_USER, "OTA CHECK");
    owner_command(sms, OTHER_USER, "OTA STATUS");
    owner_command(sms, OWNER, "OTA CONFIRM 123");
    assert(sent == 1 && strstr(last_reply, "Use OTA CHECK") && queued == OTA_SMS_NONE);
    fail_send = is_case("sms-reply-failed");
    owner_command(sms, OWNER, "OTA CHECK");
    assert(strstr(last_reply, "Checking") && !requests && !touched_sectors && !selections);
    if (fail_send) {
        assert(queued == OTA_SMS_NONE);
        return;
    }
    sms->pending_acks = 1;
    assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem && requests == 0);
    sms->pending_acks = 0;
    if (is_case("sms-ack-failed")) {
        ota_sms_ack_failed();
        assert(queued == OTA_SMS_NONE && !requests);
        return;
    }
    assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
    assert(requests == 1 && !touched_sectors && !selections && !restarted && !durable_size);
    if (is_case("sms-missing") || is_case("sms-invalid") || is_case("sms-malformed-pointer") ||
        is_case("sms-equal") || is_case("sms-older")) {
        assert(!challenge.expires);
        assert(strstr(last_reply, is_case("sms-missing") ? "No latest release" :
               (is_case("sms-equal") || is_case("sms-older")) ? "No newer update" : "check failed"));
        return;
    }

    assert(challenge.expires && strstr(last_reply, "Signed update v2 available"));
    char confirm[32];
    snprintf(confirm, sizeof(confirm), "OTA CONFIRM %s", challenge.code);
    unsigned prior = sent;
    owner_command(sms, OTHER_USER, confirm);
    owner_command(sms, OTHER_USER, "OTA CANCEL");
    assert(sent == prior && challenge.expires && queued == OTA_SMS_NONE);
    owner_command(sms, OWNER, "OTA STATUS");
    assert(strstr(last_reply, "Awaiting confirmation"));
    if (is_case("sms-cancel") || is_case("sms-expiry")) {
        if (is_case("sms-cancel")) {
            owner_command(sms, OWNER, "OTA CANCEL");
            assert(strstr(last_reply, "cancelled"));
        } else {
            clock_adjustment += OTA_SMS_LIFETIME_US;
        }
        owner_command(sms, OWNER, confirm);
        assert(strstr(last_reply, "Invalid or expired") && queued == OTA_SMS_NONE);
        assert(requests == 1 && !touched_sectors);
        return;
    }

    if (is_case("sms-replay")) {
        owner_command(sms, OWNER, "OTA CHECK");
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(challenge.expires && strncmp(confirm + 12, challenge.code, OTA_SMS_CODE_LENGTH));
        owner_command(sms, OWNER, confirm);
        assert(strstr(last_reply, "Invalid or expired") && queued == OTA_SMS_NONE && requests == 2);
        return;
    }

    fail_commit = is_case("sms-journal-failed");
    owner_command(sms, OWNER, confirm);
    assert(strstr(last_reply, "Confirmed v2") && requests == 1 && !touched_sectors);
    assert(!challenge.expires && queued == OTA_SMS_CONFIRM);
    assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
    owner_command(sms, OWNER, confirm);
    assert(strstr(last_reply, "Invalid or expired") && queued == OTA_SMS_NONE);

    if (is_case("sms-changed")) {
        assert(requests == 5 && !touched_sectors && !selection_attempts && !restarted);
        return;
    }
    assert(requests == 7 && touched_sectors == 3);
    if (is_case("sms-journal-failed") || is_case("sms-selection-failed")) {
        assert(!selections && !restarted && !durable_size);
        return;
    }
    assert(selections == 1 && restarted == 1 && durable_size);
    if (is_case("sms-rollback")) {
        selected = running;
    } else {
        running = selected;
        running_version = "2";
        running_state = ESP_OTA_IMG_PENDING_VERIFY;
    }
    prior = sent;
    sms->configured = false;
    ota_sms_report_boot(sms, &fixture_modem);
    assert(sent == prior && durable_size);
    sms->configured = true;
    if (!is_case("sms-rollback")) {
        ota_sms_report_boot(sms, &fixture_modem);
        assert(sent == prior && durable_size);
        running_state = ESP_OTA_IMG_VALID;
    }
    if (is_case("sms-readback")) {
        flash[running->address + 999] ^= 1;
    }
    if (is_case("sms-report-commit-failed")) {
        fail_commit = true;
        ota_sms_report_boot(sms, &fixture_modem);
        assert(sent == prior && durable_size);
        fail_commit = false;
    }
    if (is_case("sms-owner-changed")) {
        sms->owner = OTHER_USER;
    }
    fail_send = is_case("sms-report-failed");
    ota_sms_report_boot(sms, &fixture_modem);
    assert(sent == prior + !is_case("sms-owner-changed") && !durable_size);
    if (!is_case("sms-owner-changed")) {
        assert(strstr(last_reply, is_case("sms-rollback") || is_case("sms-readback")
                      ? "did not validate" : "Updated successfully to v2"));
    }
    ota_sms_report_boot(sms, &fixture_modem);
    assert(sent == prior + !is_case("sms-owner-changed"));
}
