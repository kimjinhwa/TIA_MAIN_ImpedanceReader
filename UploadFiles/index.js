"use strict";

const TEST_ESP32_HOST = "192.168.11.1";
const isLocalPreview = document.location.hostname.includes("127.0.0") || document.location.hostname === "localhost";
const apiBaseUrl = isLocalPreview ? ("http://" + TEST_ESP32_HOST) : "";
const DEV_AUTH_BYPASS = true;

function apiUrl(path) {
    return apiBaseUrl + path;
}

function getCookie(name) {
    const nameEQ = name + "=";
    const cookies = document.cookie.split(";");
    for (let i = 0; i < cookies.length; i++) {
        let cookie = cookies[i];
        while (cookie.charAt(0) === " ") cookie = cookie.substring(1);
        if (cookie.indexOf(nameEQ) === 0) return cookie.substring(nameEQ.length);
    }
    return null;
}

function setCookie(name, value, expires) {
    document.cookie = name + "=" + value + "; expires=" + expires.toUTCString() + "; path=/";
}

function setText(id, value) {
    const el = document.getElementById(id);
    if (el) el.textContent = value;
}

function formatNumber(value, digits) {
    if (typeof value !== "number" || Number.isNaN(value)) return "-";
    return value.toFixed(digits);
}

function numberOrNull(value) {
    if (typeof value !== "number" || Number.isNaN(value)) return null;
    return value;
}

function pickDeviceNo(devicePayload, fallback) {
    const modbusId = numberOrNull(devicePayload?.modbusId);
    if (modbusId !== null) return modbusId;
    return fallback;
}

function parseBatteryRows(payload) {
    const devices = payload?.data?.multi_data?.devices || {};
    const rackNo = numberOrNull(payload?.data?.rackInfo?.rackno);
    const keys = Object.keys(devices);
    if (keys.length === 0) return [];

    const rows = [];
    keys.forEach(function (key, index) {
        const item = devices[key] || {};
        const values = Array.isArray(item?.data?.data) ? item.data.data : [];

        const explicitTemp = numberOrNull(item?.data?.temperature) ?? numberOrNull(item?.temperature);
        const explicitCurrent = numberOrNull(item?.data?.current) ?? numberOrNull(item?.current);

        const fallbackNo = (index === 0 && rackNo !== null) ? rackNo : Number(key);
        const deviceNo = pickDeviceNo(item?.data || item, fallbackNo);
        const half = Math.floor(values.length / 2);
        const totalCells = Math.max(
            1,
            Math.min(15, half)
        );

        for (let i = 0; i < totalCells; i++) {
            const rawV = numberOrNull(values[i]);
            const rawZ = numberOrNull(values[half + i]);
            rows.push({
                deviceNo: deviceNo,
                voltageV: rawV === null ? null : rawV / 1000.0,
                impedanceMohm: rawZ === null ? null : rawZ / 100.0,
                temperatureC: explicitTemp,
                currentA: explicitCurrent
            });
        }
    });

    return rows;
}

function renderBatteryTable(rows) {
    const body = document.getElementById("batteryTableBody");
    if (!body) return;

    if (!Array.isArray(rows) || rows.length === 0) {
        body.innerHTML = "<tr><td colspan='5'>표시할 배터리 데이터가 없습니다.</td></tr>";
        return;
    }

    let html = "";
    for (let i = 0; i < rows.length; i++) {
        const row = rows[i];
        html += "<tr>"
            + "<td>" + (row.deviceNo ?? "-") + "</td>"
            + "<td>" + formatNumber(row.voltageV, 3) + "</td>"
            + "<td>" + formatNumber(row.impedanceMohm, 2) + "</td>"
            + "<td>" + formatNumber(row.temperatureC, 1) + "</td>"
            + "<td>" + formatNumber(row.currentA, 1) + "</td>"
            + "</tr>";
    }
    body.innerHTML = html;
}

async function ensureAuthenticated() {
    if (DEV_AUTH_BYPASS) return true;
    if (isLocalPreview) {
        const loginCookie = getCookie("login");
        if (loginCookie && new Date() < new Date(loginCookie)) return true;
        window.location.href = "login.html";
        return false;
    }
    try {
        const response = await fetch(apiUrl("/api/me"), { method: "GET", credentials: "include" });
        if (response.status === 401 || response.status === 403) {
            window.location.href = "login.html";
            return false;
        }
        if (!response.ok) {
            setText("statusText", "인증 확인 실패: " + response.status);
            return false;
        }
        return true;
    } catch (error) {
        console.log(error);
        setText("statusText", "인증 확인 네트워크 오류");
        return false;
    }
}

async function loadBatterySummary() {
    setText("statusText", "배터리 데이터 조회 중...");
    try {
        const response = await fetch(apiUrl("/api/battery"), { method: "GET", credentials: "include" });
        if (!response.ok) {
            setText("statusText", "조회 실패: " + response.status);
            return;
        }
        const payload = await response.json();
        const rows = parseBatteryRows(payload);
        renderBatteryTable(rows);
        setText("lastUpdated", new Date().toLocaleString());
        setText("statusText", "조회 완료");
    } catch (error) {
        console.log(error);
        setText("statusText", "네트워크 오류");
    }
}

async function logout() {
    try {
        await fetch(apiUrl("/api/logout"), { method: "POST", credentials: "include" });
    } catch (error) {
        console.log(error);
    }
    setCookie("login", "", new Date(0));
    window.location.href = "login.html";
}

window.addEventListener("load", async function () {
    const ok = await ensureAuthenticated();
    if (!ok) return;

    const refreshBtn = document.getElementById("refreshBtn");
    const logoutBtn = document.getElementById("logout");
    if (refreshBtn) refreshBtn.addEventListener("click", loadBatterySummary);
    if (logoutBtn) logoutBtn.addEventListener("click", logout);

    await loadBatterySummary();
    setInterval(loadBatterySummary, 3000);
});