"use strict";

const TEST_ESP32_HOST = "192.168.11.1";
const isLocalPreview = document.location.hostname.includes("127.0.0") || document.location.hostname === "localhost";
const apiBaseUrl = isLocalPreview ? ("http://" + TEST_ESP32_HOST) : "";
const DEV_AUTH_BYPASS = true;

function apiUrl(path) {
    return apiBaseUrl + path;
}

function setCookie(name, value, expires) {
    document.cookie = name + "=" + value + "; expires=" + expires.toUTCString() + "; path=/";
}

function getCookie(name) {
    const nameEQ = name + "=";
    const cookies = document.cookie.split(";");
    for (let i = 0; i < cookies.length; i++) {
        let cookie = cookies[i];
        while (cookie.charAt(0) === " ") {
            cookie = cookie.substring(1);
        }
        if (cookie.indexOf(nameEQ) === 0) {
            return cookie.substring(nameEQ.length);
        }
    }
    return null;
}

function setText(id, value) {
    const el = document.getElementById(id);
    if (!el) return;
    el.textContent = value;
}

function clampInt(v, min, max) {
    if (!Number.isFinite(v)) return null;
    const n = Math.round(v);
    if (n < min || n > max) return null;
    return n;
}

async function loadBmsConfig() {
    try {
        setText("statusText", "BMS 설정 조회중...");
        const response = await fetch(apiUrl("/api/bms-config"), {
            method: "GET",
            credentials: "include"
        });
        if (!response.ok) {
            setText("statusText", "BMS 설정 조회 실패: " + response.status);
            return;
        }
        const payload = await response.json();
        const cfg = payload.bmsControl || {};
        const percent = Number(cfg.impedanceEepromChangePercent);
        const period = Number(cfg.impedanceMeasurePeriodSec);
        const readMax = Number(cfg.impedanceReadMax);
        const stableWindow = Number(cfg.impedanceStableWindow);
        const stableTol = Number(cfg.impedanceStableTolPercent);
        const postSamples = Number(cfg.impedancePostStableSamples);
        const minValidMohm = Number(cfg.impedanceMinValidMohm);

        const percentInput = document.getElementById("impChangePercentInput");
        const periodInput = document.getElementById("impPeriodSecInput");
        const readMaxInput = document.getElementById("impReadMaxInput");
        const stableWindowInput = document.getElementById("impStableWindowInput");
        const stableTolInput = document.getElementById("impStableTolPercentInput");
        const postSamplesInput = document.getElementById("impPostStableSamplesInput");
        const minValidInput = document.getElementById("impMinValidMohmInput");
        if (percentInput && Number.isFinite(percent)) percentInput.value = String(percent);
        if (periodInput && Number.isFinite(period)) periodInput.value = String(period);
        if (readMaxInput && Number.isFinite(readMax)) readMaxInput.value = String(readMax);
        if (stableWindowInput && Number.isFinite(stableWindow)) stableWindowInput.value = String(stableWindow);
        if (stableTolInput && Number.isFinite(stableTol)) stableTolInput.value = String(stableTol);
        if (postSamplesInput && Number.isFinite(postSamples)) postSamplesInput.value = String(postSamples);
        if (minValidInput && Number.isFinite(minValidMohm)) minValidInput.value = minValidMohm.toFixed(1);

        setText("statusText", "BMS 설정 조회 완료");
    } catch (error) {
        console.log(error);
        setText("statusText", "BMS 설정 조회 네트워크 오류");
    }
}

async function saveBmsConfig() {
    const percentRaw = Number(document.getElementById("impChangePercentInput")?.value || "0");
    const periodRaw = Number(document.getElementById("impPeriodSecInput")?.value || "0");
    const readMaxRaw = Number(document.getElementById("impReadMaxInput")?.value || "0");
    const stableWindowRaw = Number(document.getElementById("impStableWindowInput")?.value || "0");
    const stableTolRaw = Number(document.getElementById("impStableTolPercentInput")?.value || "0");
    const postSamplesRaw = Number(document.getElementById("impPostStableSamplesInput")?.value || "0");
    const minValidMohmRaw = Number(document.getElementById("impMinValidMohmInput")?.value || "0");
    const percent = clampInt(percentRaw, 1, 100);
    const period = clampInt(periodRaw, 1, 65535);
    const readMax = clampInt(readMaxRaw, 1, 120);
    const stableWindow = clampInt(stableWindowRaw, 2, 20);
    const stableTol = clampInt(stableTolRaw, 1, 20);
    const postSamples = clampInt(postSamplesRaw, 1, 20);
    const minValidMohm = Number.isFinite(minValidMohmRaw) ? Math.round(minValidMohmRaw * 10) / 10 : NaN;

    if (percent === null || period === null || readMax === null || stableWindow === null || stableWindow > readMax || stableTol === null || postSamples === null || !Number.isFinite(minValidMohm) || minValidMohm < 0.1 || minValidMohm > 1000) {
        setText("statusText", "입력 범위: 임계치(1~100), 주기(1~65535), 최대읽기(1~120), 윈도우(2~20, <=최대읽기), 안정도(1~20), 추가샘플(1~20), 최소mΩ(0.1~1000)");
        return;
    }

    try {
        setText("statusText", "BMS 설정 저장중...");
        const response = await fetch(apiUrl("/api/bms-config"), {
            method: "POST",
            credentials: "include",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
                bmsControl: {
                    impedanceEepromChangePercent: percent,
                    impedanceMeasurePeriodSec: period,
                    impedanceReadMax: readMax,
                    impedanceStableWindow: stableWindow,
                    impedanceStableTolPercent: stableTol,
                    impedancePostStableSamples: postSamples,
                    impedanceMinValidMohm: minValidMohm
                }
            })
        });
        if (!response.ok) {
            let reason = "";
            try { reason = await response.text(); } catch (e) {}
            setText("statusText", "BMS 설정 저장 실패: " + response.status + (reason ? " (" + reason + ")" : ""));
            return;
        }
        await loadBmsConfig();
        setText("statusText", "BMS 설정 저장 완료");
    } catch (error) {
        console.log(error);
        setText("statusText", "BMS 설정 저장 네트워크 오류");
    }
}

async function runSystemAction(action, confirmMessage, successMessage) {
    const ok = window.confirm(confirmMessage + "\n\n진행하시겠습니까? (yes/no)");
    if (!ok) {
        setText("statusText", "작업 취소됨");
        return;
    }

    try {
        setText("statusText", "시스템 작업 요청중...");
        const response = await fetch(apiUrl("/api/system-action"), {
            method: "POST",
            credentials: "include",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ action: action })
        });
        const payload = await response.json();
        if (!response.ok || !payload.ok) {
            setText("statusText", "시스템 작업 실패: " + (payload.error || response.status));
            return;
        }
        setText("statusText", successMessage);
    } catch (error) {
        console.log(error);
        setText("statusText", "시스템 작업 네트워크 오류");
    }
}

function setBaselineStatus(text) {
    setText("impBaselineStatusText", text);
}

async function checkImpedanceBaselineStatus() {
    try {
        const response = await fetch(apiUrl("/api/impedance-baseline/status"), {
            method: "GET",
            credentials: "include"
        });
        if (!response.ok) {
            setBaselineStatus("진행상태 조회 실패: " + response.status);
            return;
        }
        const payload = await response.json();
        if (!payload.ok) {
            setBaselineStatus("진행상태 조회 실패");
            return;
        }
        if (payload.running) {
            setBaselineStatus(
                "진행중: " + payload.progressCell + "/" + payload.totalCells +
                " (" + payload.percent + "%)"
            );
        } else {
            setBaselineStatus("대기/완료 상태 (총 " + payload.totalCells + "셀)");
        }
    } catch (error) {
        console.log(error);
        setBaselineStatus("진행상태 조회 네트워크 오류");
    }
}

async function startImpedanceBaseline() {
    const ok = window.confirm(
        "충전기 분리 후 실행하세요.\n유효 내부저항을 EEPROM 기준값으로 순차 저장합니다.\n\n진행하시겠습니까?"
    );
    if (!ok) {
        setBaselineStatus("작업 취소됨");
        return;
    }

    try {
        setBaselineStatus("시작 요청중...");
        const response = await fetch(apiUrl("/api/impedance-baseline/start"), {
            method: "POST",
            credentials: "include",
            headers: { "Content-Type": "application/json" },
            body: "{}"
        });
        if (!response.ok) {
            let reason = "";
            try { reason = await response.text(); } catch (e) {}
            setBaselineStatus("시작 실패: " + response.status + (reason ? " (" + reason + ")" : ""));
            return;
        }
        setBaselineStatus("시작됨. 진행상태를 조회하세요.");
        await checkImpedanceBaselineStatus();
    } catch (error) {
        console.log(error);
        setBaselineStatus("시작 요청 네트워크 오류");
    }
}

async function ensureAuthenticated() {
    if (DEV_AUTH_BYPASS) return true;
    if (isLocalPreview) {
        const loginCookie = getCookie("login");
        if (loginCookie && new Date() < new Date(loginCookie)) {
            return true;
        }
        window.location.href = "login.html";
        return false;
    }

    try {
        const response = await fetch(apiUrl("/api/me"), {
            method: "GET",
            credentials: "include"
        });
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

async function logout() {
    try {
        await fetch(apiUrl("/api/logout"), {
            method: "POST",
            credentials: "include"
        });
    } catch (error) {
        console.log(error);
    }
    setCookie("login", "", new Date(0));
    window.location.href = "login.html";
}

window.addEventListener("load", async function () {
    const ok = await ensureAuthenticated();
    if (!ok) return;

    const logoutBtn = document.getElementById("logout");
    if (logoutBtn) {
        logoutBtn.addEventListener("click", logout);
    }

    const loadBmsConfigBtn = document.getElementById("loadBmsConfigBtn");
    const saveBmsConfigBtn = document.getElementById("saveBmsConfigBtn");
    const formatFsBtn = document.getElementById("formatFsBtn");
    const resetSystemBtn = document.getElementById("resetSystemBtn");
    const rebootSystemBtn = document.getElementById("rebootSystemBtn");
    const startBaselineBtn = document.getElementById("startBaselineBtn");
    const checkBaselineBtn = document.getElementById("checkBaselineBtn");
    if (loadBmsConfigBtn) {
        loadBmsConfigBtn.addEventListener("click", function () {
            loadBmsConfig();
        });
    }
    if (saveBmsConfigBtn) {
        saveBmsConfigBtn.addEventListener("click", function () {
            saveBmsConfig();
        });
    }
    if (formatFsBtn) {
        formatFsBtn.addEventListener("click", function () {
            runSystemAction(
                "formatFsFast",
                "파일시스템을 빠른 초기화(littleFsInitFast(1)) 합니다.\n저장된 로그/업로드 파일이 삭제될 수 있습니다.",
                "파일시스템 초기화 요청 완료"
            );
        });
    }
    if (resetSystemBtn) {
        resetSystemBtn.addEventListener("click", function () {
            runSystemAction(
                "resetSystemDefaults",
                "시스템 기본값을 EEPROM에 다시 기록합니다.\n설정값이 공장 기본값으로 변경됩니다.",
                "시스템 기본값 초기화 완료"
            );
        });
    }
    if (rebootSystemBtn) {
        rebootSystemBtn.addEventListener("click", function () {
            runSystemAction(
                "reboot",
                "시스템을 즉시 재부팅합니다.",
                "재부팅 요청 완료"
            );
        });
    }

    if (startBaselineBtn) {
        startBaselineBtn.addEventListener("click", function () {
            startImpedanceBaseline();
        });
    }
    if (checkBaselineBtn) {
        checkBaselineBtn.addEventListener("click", function () {
            checkImpedanceBaselineStatus();
        });
    }

    setBaselineStatus("대기중");
    setText("statusText", "조회 버튼으로 설정값을 읽어오세요");
});