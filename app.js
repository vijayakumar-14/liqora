import { initializeApp } from "https://www.gstatic.com/firebasejs/13.1.0/firebase-app.js";
import { getDatabase, onValue, ref } from "https://www.gstatic.com/firebasejs/13.1.0/firebase-database.js";

const firebaseConfig = {
  apiKey: "AIzaSyD3Q_CMuMgIZKVn-09KMzkWqnjQUCP6nlA",
  authDomain: "test-base-2fb81.firebaseapp.com",
  databaseURL: "https://test-base-2fb81-default-rtdb.firebaseio.com",
  projectId: "test-base-2fb81",
  storageBucket: "test-base-2fb81.firebasestorage.app",
  messagingSenderId: "762253667867",
  appId: "1:762253667867:web:f0f98e60e83b30555d12be",
  measurementId: "G-9WTY2SLE6H"
};

const app = initializeApp(firebaseConfig);
const database = getDatabase(app);
const telemetryRef = ref(database, "liquora/telemetry");

const $ = (id) => document.getElementById(id);
const connectionStatus = $("connectionStatus");

function setText(id, text) {
  $(id).textContent = text;
}

function showLevel(id, barId, value) {
  const number = value === null || value === undefined ? NaN : Number(value);
  if (!Number.isFinite(number)) {
    setText(id, "—");
    $(barId).style.width = "0%";
    return;
  }
  const percent = Math.max(0, Math.min(100, number));
  setText(id, percent.toFixed(1));
  $(barId).style.width = `${percent}%`;
}

function showState(id, value) {
  const element = $(id);
  const active = Boolean(value);
  element.textContent = active ? "ON" : "OFF";
  element.className = active ? "on" : "off";
}

onValue(
  telemetryRef,
  (snapshot) => {
    const data = snapshot.val();
    if (!data) {
      connectionStatus.textContent = "Connected · waiting for ESP32";
      connectionStatus.className = "connection live";
      return;
    }

    showLevel("sumpLevel", "sumpBar", data.sumpLevel);
    showLevel("ohtLevel", "ohtBar", data.ohtLevel);
    setText("sumpFlowRate", Number(data.sumpFlowRate ?? 0).toFixed(2));
    setText("boreFlowRate", Number(data.boreFlowRate ?? 0).toFixed(2));
    setText("currentSource", data.currentSource || "NONE");

    showState("sumpPump", data.relays?.sumpPump);
    showState("borePump", data.relays?.borePump);
    showState("municipalValve", data.relays?.municipalValve);
    showState("municipalFilling", data.municipalFillingActive);
    showState("relayIn4", data.relays?.in4);
    showState("sumpAvailable", data.sumpAvailable);
    showState("ohtFilling", data.ohtFillingActive);
    showState("sumpLockout", data.sumpLockedOut);
    showState("boreLockout", data.boreLockedOut);

    setText(
      "updatedAt",
      data.updatedAt ? `Updated ${new Date(Number(data.updatedAt)).toLocaleTimeString()}` : "Updated just now"
    );
    setText("errorMessage", "");
    connectionStatus.textContent = "Live";
    connectionStatus.className = "connection live";
  },
  (error) => {
    console.error("Firebase read failed:", error);
    connectionStatus.textContent = "Firebase error";
    connectionStatus.className = "connection error";
    setText("errorMessage", "Check that the Realtime Database rules are published.");
  }
);
