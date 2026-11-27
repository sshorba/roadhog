static void owner_command(sms_control_t *sms, const char *sender, const char *text)
{
    sms_request_t request;
    snprintf(request.sender, sizeof(request.sender), "%s", sender);
    snprintf(request.text, sizeof(request.text), "%s", text);
    assert(ota_sms_service_command(sms, &fixture_modem, &request));
}

/* Deliver an SMS-DELIVER TPDU in the format used by the production UART parser. */
static void deliver_pdu(sms_control_t *sms, const char *sender, const char *text,
                        uint8_t flags, unsigned storage_index)
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
    if (storage_index) {
        snprintf(line, sizeof(line), "+CMGL: %u,0,,%zu", storage_index, used - 1);
    } else {
        snprintf(line, sizeof(line), "+CMT: ,%zu", used - 1);
    }
    assert(sms_control_notification(sms, line));
    for (size_t i = 0; i < used; ++i) {
        line[i * 2] = hex[pdu[i] >> 4];
        line[i * 2 + 1] = hex[pdu[i] & 15];
    }
    line[2 * used] = 0;
    assert(sms_control_notification(sms, line));
}

static void pdu_command(sms_control_t *sms, const char *sender, const char *text, uint8_t flags)
{
    deliver_pdu(sms, sender, text, flags, 0);
}

static void stored_pdu(sms_control_t *sms, const char *text, unsigned index)
{
    deliver_pdu(sms, OWNER, text, 0, index);
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
    fail_ack = false;
    clock_adjustment += INT64_C(5100000);
    sms_control_service(sms, &fixture_modem);
    assert(sms->configured && !sms->routing_recovery);
    pdu_command(sms, OWNER, "ota check", 0);
    sms_control_service(sms, &fixture_modem);
    assert(acks == 4 && sent == 1 && queued == OTA_SMS_CHECK && !requests);
    assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
    assert(requests == 1 && challenge.expires && !touched_sectors && !selections);
}

static void test_cnma_recovery(const modem_config_t *config, sms_control_t *sms)
{
    if (is_case("sms-truncated-header")) {
        assert(sms_control_notification(sms, "+CMT: ,"));
        assert(!sms->expected_tpdu && sms->pending_nacks == 1 && sms->delivery_fault);
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem && !requests);
        sms_control_service(sms, &fixture_modem);
        assert(nacks == 1 && queued == OTA_SMS_NONE && !requests);
        return;
    }

    fail_ack = is_case("sms-cnma-ack-recovery");
    fail_nack = !fail_ack;
    if (fail_ack) {
        pdu_command(sms, OWNER, "OTA STATUS", 0);
    } else {
        assert(sms_control_notification(sms, "+CMT: ,40"));
        clock_adjustment += INT64_C(1100000);
    }
    sms_control_service(sms, &fixture_modem);
    assert(routing_reset && modem_busy && sms->routing_recovery && !sms->configured &&
           queued == OTA_SMS_NONE && !requests && !sms->request_count);
    unsigned before = direct_readbacks;
    clock_adjustment += INT64_C(5100000);
    sms_control_service(sms, &fixture_modem);
    assert(direct_readbacks == before && sms->routing_recovery && !requests);

    late_final = true;
    assert(modem_poll(&fixture_modem, 250) == ESP_OK);
    fail_ack = fail_nack = false;
    wrong_direct_readback = true;
    sms_control_service(sms, &fixture_modem);
    assert(direct_readbacks == before + 1 && !sms->configured && sms->routing_recovery);
    int input[2];
    assert(pipe(input) == 0 && dup2(input[0], STDIN_FILENO) == STDIN_FILENO);
    assert(close(input[0]) == 0 && write(input[1], "modem inspect\n", 14) == 14);
    assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem &&
           !requests && maintenance_calls == 1);
    assert(close(input[1]) == 0);

    wrong_direct_readback = false;
    stored_status = true;
    clock_adjustment += INT64_C(5100000);
    sms_control_service(sms, &fixture_modem);
    assert(sms->configured && sms->routing_recovery && inbox_scans == 1 && stored_deletes == 1);
    owner_command(sms, OWNER, "OTA CHECK");
    assert(strstr(last_reply, "SMS routing is recovering") && queued == OTA_SMS_NONE && !requests);
    sms_control_service(sms, &fixture_modem);
    assert(sms->configured && !sms->routing_recovery && inbox_scans == 2 && direct_readbacks >= before + 4);

    pdu_command(sms, OWNER, "OTA CHECK", 0);
    sms_control_service(sms, &fixture_modem);
    assert(queued == OTA_SMS_CHECK);
    assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
    assert(requests == 1 && challenge.expires && !touched_sectors && !selection_attempts);
}

static void test_sms(const modem_config_t *config, sms_control_t *sms)
{
    ota_sms_init();
    at_ready = true;
    sms->owner = OWNER;
    if (is_case("sms-supported-cnmi")) {
        for (unsigned i = 0; i < 2; ++i) {
            assert(!command_ok(&fixture_modem, "AT+CNMI=0,2,0,0,0"));
        }
    }
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
    if (is_case("sms-cnma-nack-recovery") || is_case("sms-cnma-ack-recovery") ||
        is_case("sms-truncated-header")) {
        test_cnma_recovery(config, sms);
        return;
    }
    if (is_case("sms-incomplete-expiry")) {
        int input[2];
        assert(pipe(input) == 0 && dup2(input[0], STDIN_FILENO) == STDIN_FILENO);
        assert(close(input[0]) == 0 && write(input[1], "modem inspect\n", 14) == 14);
        assert(sms_control_notification(sms, "+CMT: ,40"));
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(!maintenance_calls && !requests && sms->expected_tpdu);
        clock_adjustment += INT64_C(1100000);
        assert(esp_timer_get_time() < queued_until);
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(!requests && queued == OTA_SMS_CHECK && sms->pending_nacks == 1);
        sms_control_service(sms, &fixture_modem);
        assert(nacks == 1 && !sms->pending_nacks);
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(maintenance_calls == 1 && !requests && !sms->expected_tpdu && queued == OTA_SMS_NONE);
        assert(sms_control_notification(sms, "00112233"));
        assert(!sms->request_count && !sms->pending_acks);
        assert(close(input[1]) == 0);
        return;
    }
    if (is_case("sms-cnmi-readback") || is_case("sms-me-full")) {
        for (unsigned i = 0; i < 2; ++i) {
            assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
            assert(!requests && !storage_mode && queued == OTA_SMS_CHECK);
        }
        return;
    }
    if (is_case("sms-quiesce-failed")) {
        fail_quiesce = true;
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(!requests && uart_open && sms->configured && queued == OTA_SMS_CHECK);
        fail_quiesce = false;
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
    unsigned discovery_requests = is_case("sms-latest-redirect") || is_case("sms-latest-wrong-cert") ? 2 : 1;
    assert(requests == discovery_requests && !touched_sectors && !selections && !restarted && !durable_size);
    if (is_case("sms-latest-redirect") || is_case("sms-quiesce-failed")) {
        assert(challenge.expires && strstr(last_reply, "Signed update v2 available"));
        return;
    }
    if (is_case("sms-missing") || is_case("sms-invalid") || is_case("sms-malformed-pointer") ||
        is_case("sms-equal") || is_case("sms-older") || is_case("sms-legacy") ||
        is_case("sms-latest-wrong-cert")) {
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

    if (is_case("sms-reboot-replay")) {
        challenge = (ota_sms_challenge_t){0};
        ota_sms_init();
        pdu_command(sms, OWNER, confirm, 0);
        sms_control_service(sms, &fixture_modem);
        assert(strstr(last_reply, "Invalid or expired") && queued == OTA_SMS_NONE && requests == 1);
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

    if (is_case("sms-confirm-overflow")) {
        pdu_command(sms, OWNER, confirm, 0);
        for (unsigned i = 1; i < SMS_QUEUE_CAPACITY; ++i) {
            pdu_command(sms, OWNER, "OTA STATUS", 0);
        }
        pdu_command(sms, OWNER, "OTA CANCEL", 0);
        sms_control_service(sms, &fixture_modem);
        sms_control_service(sms, &fixture_modem);
        assert(nacks == 1 && !challenge.expires && queued == OTA_SMS_NONE && requests == 1);
        assert(strstr(last_reply, "Invalid or expired") && !selection_attempts);
        return;
    }

    fail_commit = is_case("sms-journal-failed");
    fail_send = is_case("sms-confirm-reply-failed");
    unsigned before_ack = acks;
    reply_requires_ack = before_ack + 1;
    pdu_command(sms, OWNER, confirm, 0);
    sms_control_service(sms, &fixture_modem);
    assert(acks == before_ack + 1 && strstr(last_reply, "Confirmed v2") &&
           requests == 1 && !touched_sectors && !challenge.expires);
    if (fail_send) {
        assert(queued == OTA_SMS_NONE);
        fail_send = false;
        owner_command(sms, OWNER, confirm);
        assert(strstr(last_reply, "Invalid or expired"));
        return;
    }
    assert(queued == OTA_SMS_CONFIRM);
    if (is_case("sms-queued-expiry")) {
        clock_adjustment += OTA_SMS_LIFETIME_US;
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(queued == OTA_SMS_NONE && requests == 1 && !touched_sectors);
        return;
    }
    if (is_case("sms-boundary-cancel")) {
        boundary_delivery = sms;
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(queued == OTA_SMS_CONFIRM && requests == 1 && sms->pending_acks && uart_open);
        sms_control_service(sms, &fixture_modem);
        assert(strstr(last_reply, "cancelled") && queued == OTA_SMS_NONE && !sms->pending_acks);
        return;
    }
    if (is_case("sms-recovery-cancel") || is_case("sms-recovery-ack-failed")) {
        resume_delivery = sms;
    }
    assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
    assert(!selections && !restarted && !durable_size);
    if (is_case("sms-changed") || is_case("sms-wrong-sequence") || is_case("sms-wrong-target") ||
        is_case("sms-incompatible")) {
        assert(requests == (is_case("sms-incompatible") ? 7 : 5));
        assert(!touched_sectors && !selection_attempts && !staged.install);
        return;
    }
    assert(requests == 7 && touched_sectors == 3);
    if (is_case("sms-resume-failed")) {
        assert(!sms->configured && !staged.install && !selection_attempts && uart_open);
        return;
    }
    assert(staged.install && !storage_mode);
    if (is_case("sms-incomplete-staged") || is_case("sms-overflow")) {
        if (is_case("sms-incomplete-staged")) {
            assert(sms_control_notification(sms, "+CMT: ,40"));
            clock_adjustment += INT64_C(1100000);
            assert(esp_timer_get_time() < staged_until);
        } else {
            for (unsigned i = 0; i < SMS_QUEUE_CAPACITY; ++i) {
                pdu_command(sms, OWNER, "OTA STATUS", 0);
            }
            pdu_command(sms, OWNER, "OTA CANCEL", 0);
            assert(sms->request_count == SMS_QUEUE_CAPACITY && sms->pending_nacks == 1);
        }
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(staged.install && !selection_attempts && !restarted);
        sms_control_service(sms, &fixture_modem);
        sms_control_service(sms, &fixture_modem);
        assert(nacks == 1 && !staged.install && !selection_attempts && !restarted);
        return;
    }
    if (is_case("sms-cancel-delete-late")) {
        stored_cancel = true;
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(staged.install && stored_cancel && !selection_attempts);
        sms_control_service(sms, &fixture_modem);
        assert(!staged.install && !stored_cancel && stored_deletes == 1 && modem_busy);
        late_final = true;
        assert(modem_poll(&fixture_modem, 250) == ESP_OK);
        clock_adjustment += INT64_C(5100000);
        sms_control_service(sms, &fixture_modem);
        assert(sms->configured && !sms->storage_pending && !modem_busy);
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(!selection_attempts && !restarted && selected == running);
        return;
    }
    if (is_case("sms-stored-cancel") || is_case("sms-stored-fifo") || is_case("sms-status-delete-failed")) {
        stored_cancel = true;
        stored_status = is_case("sms-stored-fifo") || is_case("sms-status-delete-failed");
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(staged.install && stored_cancel && !storage_mode && !selection_attempts);
        unsigned before = acks;
        unsigned expected_deletes = 1;
        if (is_case("sms-status-delete-failed")) {
            unsigned before_reply = sent;
            fail_delete = true;
            sms_control_service(sms, &fixture_modem);
            assert(sent == before_reply && stored_status && stored_cancel && !stored_deletes && !staged.install);
            fail_delete = false;
        }
        if (stored_status) {
            sms_control_service(sms, &fixture_modem);
            bool status_failure = is_case("sms-status-delete-failed");
            assert(strstr(last_reply, status_failure ? "acknowledgement failure" : "Image verified") &&
                   !stored_status && stored_cancel);
            assert(staged.install == !status_failure && stored_deletes == 1);
            assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
            assert(staged.install == !status_failure && !selection_attempts);
            expected_deletes = 2;
        }
        sms_control_service(sms, &fixture_modem);
        assert(!staged.install && stored_deletes == expected_deletes && !stored_cancel && acks == before);
        assert(strstr(last_reply, is_case("sms-status-delete-failed") ?
                      "No pending OTA request" : "cancelled") && !selection_attempts && !restarted);
        return;
    }
    if (is_case("sms-recovery-cancel") || is_case("sms-recovery-ack-failed")) {
        assert(sms->pending_acks && sms->request_count);
        assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
        assert(!selection_attempts && !restarted);
        fail_ack = is_case("sms-recovery-ack-failed");
        sms_control_service(sms, &fixture_modem);
        assert((fail_ack || strstr(last_reply, "cancelled")) && !staged.install && !sms->pending_acks);
        assert(!selection_attempts && !restarted);
        return;
    }
    if (is_case("sms-reset-before-selection")) {
        nvs_handle_t store;
        assert(ota_notification_save(&staged.release, staged.target->address, OWNER, &store) == ESP_OK);
        nvs_close(store);
        staged = (update_t){0};
        assert(!selection_attempts && selected == running && durable_size);
        ota_sms_report_boot(sms, &fixture_modem);
        assert(strstr(last_reply, "did not validate") && !durable_size);
        return;
    }
    assert(ota_update_poll(&fixture_modem, config, sms) == &fixture_modem);
    owner_command(sms, OWNER, confirm);
    assert(strstr(last_reply, "Invalid or expired") && queued == OTA_SMS_NONE);
    if (is_case("sms-journal-failed") || is_case("sms-selection-failed") || is_case("sms-nvs-set-failed")) {
        assert(!selections && !restarted && !durable_size);
        return;
    }
    assert(selections == 1 && restarted == 1 && durable_size);
    if (is_case("sms-supported-cnmi")) {
        assert(forbidden_cnmi == 2 && storage_modes == 3);
    }
    if (is_case("sms-late-cancel")) {
        *sms = (sms_control_t){
            .owner = OWNER, .configured = true, .storage_pending = true, .storage_startup = true,
        };
        remote_status = NULL;
        storage_mode = false;
        running = selected;
        running_version = "2";
        owner_command(sms, OWNER, "OTA CHECK");
        assert(strstr(last_reply, "Reconciling stored SMS") && !challenge.expires && queued == OTA_SMS_NONE);
        sms_control_service(sms, &fixture_modem);
        assert(!stored_cancel && stored_deletes == 1 && sent > prior &&
               strstr(last_reply, "No pending OTA request"));
        unsigned after = sent;
        sms_control_service(sms, &fixture_modem);
        assert(sent == after && durable_size && !sms->storage_startup);
        return;
    }
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
        assert(editor_aborts == 1);
        fail_commit = false;
    }
    if (is_case("sms-pre-prompt-retry")) {
        reject_prompt = true;
        ota_sms_report_boot(sms, &fixture_modem);
        assert(pre_prompts == 1 && !editor_aborts && sent == prior && durable_size);
        reject_prompt = false;
    }
    if (is_case("sms-report-busy") || is_case("sms-nvs-erase-failed")) {
        if (is_case("sms-report-busy")) {
            gps_timeout = true;
            assert(!start_gps(&fixture_modem));
        }
        fail_erase = is_case("sms-nvs-erase-failed");
        ota_sms_report_boot(sms, &fixture_modem);
        assert(sent == prior && durable_size);
        if (is_case("sms-report-busy")) {
            late_final = true;
            assert(modem_poll(&fixture_modem, 250) == ESP_OK && !modem_busy);
            assert(start_gps(&fixture_modem));
        }
        assert(editor_aborts == (unsigned)is_case("sms-nvs-erase-failed"));
        fail_erase = false;
    }
    if (is_case("sms-reset-before-send")) {
        cut_before_submit = true;
        ota_sms_report_boot(sms, &fixture_modem);
        assert(sent == prior && !durable_size);
        cut_before_submit = false;
        ota_sms_report_boot(sms, &fixture_modem);
        assert(sent == prior);
        return;
    }
    if (is_case("sms-owner-changed")) {
        sms->owner = OTHER_USER;
    }
    fail_send = is_case("sms-report-failed");
    missing_sms_final = is_case("sms-no-final");
    ota_sms_report_boot(sms, &fixture_modem);
    assert(sent == prior + !is_case("sms-owner-changed") && !durable_size);
    if (!is_case("sms-owner-changed")) {
        assert(strstr(last_reply, is_case("sms-rollback") || is_case("sms-readback")
                      ? "did not validate" : "Updated successfully to v2"));
    }
    unsigned attempts = pre_prompts;
    ota_sms_report_boot(sms, &fixture_modem);
    assert(sent == prior + !is_case("sms-owner-changed") && pre_prompts == attempts);
}
