const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const html = fs.readFileSync(path.join(__dirname, '../../main/portal/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];

const byId = new Map();
function element() {
    const node = {
        hidden: false,
        textContent: '',
        value: '',
        disabled: false,
        readOnly: false,
        open: false,
        children: [],
        listeners: {},
        reset() {},
        reportValidity() { return true; },
        focus() {},
        addEventListener(type, fn) { this.listeners[type] = fn; },
        append(...nodes) { this.children.push(...nodes); },
        replaceChildren(...nodes) { this.children = nodes; },
        querySelector() { return element(); },
    };
    node.elements = new Proxy({}, {
        get(target, prop) {
            if (!target[prop]) target[prop] = element();
            return target[prop];
        },
    });
    return node;
}

function response(status, body) {
    const text = typeof body === 'string' ? body : JSON.stringify(body);
    return {
        status,
        ok: status >= 200 && status < 300,
        async json() { return JSON.parse(text); },
        async text() { return text; },
    };
}

const waits = [];
const timers = [];
const context = {
    console,
    AbortController,
    setTimeout(fn, ms) {
        const timer = { fn, ms, cleared: false };
        timers.push(timer);
        return timer;
    },
    clearTimeout(timer) {
        if (timer) timer.cleared = true;
    },
    fetch(url, options = {}) {
        return new Promise((resolve) => {
            waits.push({ url, options, resolve });
        });
    },
    confirm() { return true; },
    sessionStorage: { getItem() { return ''; }, setItem() {} },
    document: {
        getElementById(id) {
            if (!byId.has(id)) byId.set(id, element());
            return byId.get(id);
        },
        createElement() { return element(); },
    },
};
vm.createContext(context);
vm.runInContext(script, context);

function listText() {
    return byId.get('sms-users').children.map((child) => child.textContent ||
        (child.children || []).map((part) => part.textContent).join('')).join('\n');
}

function userButtons() {
    const found = [];
    for (const child of byId.get('sms-users').children) {
        for (const part of child.children || []) {
            if (part.listeners && part.listeners.click) found.push(part);
        }
    }
    return found;
}

async function settle() {
    for (let i = 0; i < 20; i++) await Promise.resolve();
}

async function take(url) {
    for (let i = 0; i < 20 && !waits.some((item) => item.url === url); i++) await Promise.resolve();
    const index = waits.findIndex((item) => item.url === url);
    assert(index >= 0, 'missing fetch ' + url);
    return waits.splice(index, 1)[0];
}

async function main() {
    const state = await take('/api/setup/state');
    state.resolve(response(200, 'configured'));
    await settle();
    const status = await take('/api/status');
    status.resolve(response(200, {
        lte: 'Home', data: 'Active', gps: 'Fix', sim: '+15555550199',
        owner: '+15555550100', wifi_password: 'secret',
    }));
    await settle();
    const firstUsers = await take('/api/users');
    firstUsers.resolve(response(200, {
        owner: '+15555550100',
        users: [{ number: '+15555550101', role: 'authorized' }],
    }));
    await settle();
    assert(listText().includes('+15555550101'));
    const remove = userButtons()[0];
    assert(remove, 'missing remove button');

    const poll = timers.find((timer) => !timer.cleared && timer.ms === 5000);
    assert(poll, 'missing status poll');
    poll.cleared = true;
    poll.fn();
    await settle();
    (await take('/api/status')).resolve(response(200, {
        lte: 'Home', data: 'Active', gps: 'Fix', sim: '+15555550199',
        owner: '+15555550100', wifi_password: 'secret',
    }));
    await settle();
    const staleUsers = await take('/api/users');

    remove.listeners.click();
    await settle();
    const removal = await take('/api/users');
    assert(removal.options.body === 'remove\n+15555550101');
    removal.resolve(response(200, 'User removed'));
    await settle();
    const freshUsers = await take('/api/users');
    freshUsers.resolve(response(200, { owner: '+15555550100', users: [] }));
    await settle();
    assert(!listText().includes('+15555550101'));

    staleUsers.resolve(response(200, {
        owner: '+15555550100',
        users: [{ number: '+15555550101', role: 'authorized' }],
    }));
    await settle();
    assert(!listText().includes('+15555550101'), listText());
    assert(listText().includes('No additional users.'));
    console.log('Stale SMS user refresh is discarded');
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});
