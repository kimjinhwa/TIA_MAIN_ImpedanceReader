"use strict";

const TEST_ESP32_HOST = "192.168.11.1";
const isLocalPreview = document.location.hostname.includes("127.0.0") || document.location.hostname === "localhost";
const apiBaseUrl = isLocalPreview ? ("http://" + TEST_ESP32_HOST) : "";
const DEV_AUTH_BYPASS = true;

function apiUrl(path) {
  return apiBaseUrl + path;
}

function setText(id, value) {
  const el = document.getElementById(id);
  if (el) el.textContent = value;
}

function fmt(v, d) {
  if (typeof v !== "number" || Number.isNaN(v)) return "-";
  return v.toFixed(d);
}

function clamp(v, min, max) {
  if (!Number.isFinite(v)) return null;
  if (v < min || v > max) return null;
  return v;
}

async function ensureAuthenticated() {
  if (DEV_AUTH_BYPASS) return true;
  try {
    const response = await fetch(apiUrl("/api/me"), { method: "GET", credentials: "include" });
    if (response.status === 401 || response.status === 403) {
      window.location.href = "login.html";
      return false;
    }
    return response.ok;
  } catch (error) {
    console.log(error);
    return false;
  }
}

function renderCellRows(cells) {
  const body = document.getElementById("cellTuneBody");
  if (!body) return;
  if (!Array.isArray(cells) || cells.length === 0) {
    body.innerHTML = "<tr><td colspan='7'>데이터 없음</td></tr>";
    return;
  }
  let html = "";
  for (const c of cells) {
    html += "<tr>"
      + "<td>" + c.cellNo + "</td>"
      + "<td>" + fmt(c.currentMohm, 3) + "</td>"
      + "<td>" + fmt(c.baseMohm, 2) + "</td>"
      + "<td><input id='baseInput_" + c.cellNo + "' type='number' step='0.01' min='0.00' max='327.67' value='" + fmt(c.baseMohm, 2) + "'></td>"
      + "<td>" + fmt(c.compensationMohm, 2) + "</td>"
      + "<td><input id='compInput_" + c.cellNo + "' type='number' step='0.01' min='-327.68' max='327.67' value='" + fmt(c.compensationMohm, 2) + "'></td>"
      + "<td>"
      + "<button data-cell='" + c.cellNo + "' class='saveBaseBtn'>기준값저장</button> "
      + "<button data-cell='" + c.cellNo + "' class='saveCompBtn'>보상값저장</button>"
      + "</td>"
      + "</tr>";
  }
  body.innerHTML = html;

  document.querySelectorAll(".saveCompBtn").forEach(function (btn) {
    btn.addEventListener("click", async function () {
      const cellNo = Number(btn.getAttribute("data-cell"));
      const input = document.getElementById("compInput_" + cellNo);
      const v = Number(input?.value || "NaN");
      const comp = clamp(v, -327.68, 327.67);
      if (comp === null) {
        setText("statusText", "셀 " + cellNo + " 보정값 범위 오류");
        return;
      }
      await saveCellCompensation(cellNo, comp);
    });
  });

  document.querySelectorAll(".saveBaseBtn").forEach(function (btn) {
    btn.addEventListener("click", async function () {
      const cellNo = Number(btn.getAttribute("data-cell"));
      const input = document.getElementById("baseInput_" + cellNo);
      const v = Number(input?.value || "NaN");
      const base = clamp(v, 0.0, 327.67);
      if (base === null) {
        setText("statusText", "셀 " + cellNo + " 기준값 범위 오류(0.00~327.67)");
        return;
      }
      await saveCellBaseline(cellNo, base);
    });
  });
}

async function loadTuneData() {
  try {
    setText("statusText", "튜닝 데이터 조회중...");
    const response = await fetch(apiUrl("/api/impedance-compensation"), { method: "GET", credentials: "include" });
    if (!response.ok) {
      setText("statusText", "조회 실패: " + response.status);
      return;
    }
    const payload = await response.json();
    if (!payload.ok) {
      setText("statusText", "조회 실패");
      return;
    }
    document.getElementById("gainInput").value = fmt(payload.global?.impedanceGain, 3);
    document.getElementById("offsetInput").value = fmt(payload.global?.impedanceOffsetMohm, 2);
    renderCellRows(payload.cells || []);
    setText("statusText", "조회 완료");
  } catch (error) {
    console.log(error);
    setText("statusText", "조회 네트워크 오류");
  }
}

async function saveGlobal() {
  const gain = clamp(Number(document.getElementById("gainInput")?.value || "NaN"), 0.1, 4.0);
  const offset = clamp(Number(document.getElementById("offsetInput")?.value || "NaN"), -327.68, 327.67);
  if (gain === null || offset === null) {
    setText("statusText", "전역 gain/offset 범위 오류");
    return;
  }
  try {
    setText("statusText", "전역 보정 저장중...");
    const response = await fetch(apiUrl("/api/bms-config"), {
      method: "POST",
      credentials: "include",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ bmsControl: { impedanceGain: gain, impedanceOffsetMohm: offset } })
    });
    if (!response.ok) {
      const txt = await response.text();
      setText("statusText", "전역 저장 실패: " + response.status + " " + txt);
      return;
    }
    setText("statusText", "전역 보정 저장 완료");
    await loadTuneData();
  } catch (error) {
    console.log(error);
    setText("statusText", "전역 저장 네트워크 오류");
  }
}

async function saveCellCompensation(cellNo, compMohm) {
  try {
    setText("statusText", "셀 " + cellNo + " 보정 저장중...");
    const response = await fetch(apiUrl("/api/impedance-compensation"), {
      method: "POST",
      credentials: "include",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ cellNo: cellNo, compensationMohm: compMohm, op: "set" })
    });
    if (!response.ok) {
      const txt = await response.text();
      setText("statusText", "셀 저장 실패: " + response.status + " " + txt);
      return;
    }
    setText("statusText", "셀 " + cellNo + " 보정 저장 완료");
    await loadTuneData();
  } catch (error) {
    console.log(error);
    setText("statusText", "셀 저장 네트워크 오류");
  }
}

async function saveCellBaseline(cellNo, baseMohm) {
  try {
    setText("statusText", "셀 " + cellNo + " 기준값 저장중...");
    const response = await fetch(apiUrl("/api/impedance-compensation"), {
      method: "POST",
      credentials: "include",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ cellNo: cellNo, baseMohm: baseMohm, op: "setBase" })
    });
    if (!response.ok) {
      const txt = await response.text();
      setText("statusText", "기준값 저장 실패: " + response.status + " " + txt);
      return;
    }
    setText("statusText", "셀 " + cellNo + " 기준값 저장 완료");
    await loadTuneData();
  } catch (error) {
    console.log(error);
    setText("statusText", "기준값 저장 네트워크 오류");
  }
}

async function saveAllCompensation() {
  const allComp = clamp(Number(document.getElementById("allCompInput")?.value || "NaN"), -327.68, 327.67);
  if (allComp === null) {
    setText("statusText", "전체 보정값 범위 오류");
    return;
  }
  const ok = window.confirm("전체 셀 보정값을 " + allComp.toFixed(2) + " mOhm로 설정합니다. 진행할까요?");
  if (!ok) return;
  try {
    setText("statusText", "전체 보정 저장중...");
    const response = await fetch(apiUrl("/api/impedance-compensation"), {
      method: "POST",
      credentials: "include",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ cellNo: 0, compensationMohm: allComp, op: "set" })
    });
    if (!response.ok) {
      const txt = await response.text();
      setText("statusText", "전체 저장 실패: " + response.status + " " + txt);
      return;
    }
    setText("statusText", "전체 보정 저장 완료");
    await loadTuneData();
  } catch (error) {
    console.log(error);
    setText("statusText", "전체 저장 네트워크 오류");
  }
}

window.addEventListener("load", async function () {
  const ok = await ensureAuthenticated();
  if (!ok) return;
  document.getElementById("refreshBtn")?.addEventListener("click", loadTuneData);
  document.getElementById("saveGlobalBtn")?.addEventListener("click", saveGlobal);
  document.getElementById("applyAllBtn")?.addEventListener("click", saveAllCompensation);
  await loadTuneData();
});
