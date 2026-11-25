"use strict";

/* ========================================================================= */
/* Natural German (Default for Family/Friends) + English Toggle              */
/* ========================================================================= */
const I18N = {
  de: {
    pageTitle: "Fingerabdruck-Studie",
    stop: "Beenden",
    stopping: "Wird beendet…",
    redoBtn: "↶ Schritt zurück",
    redoing: "Wird zurückgesetzt…",
    redoTitle: "Schritt wiederholen",
    redoSub: "Letzte Eingabe verworfen — wir wiederholen diesen Schritt ganz entspannt.",
    practiceBtn: "Erst kurz üben (ca. 1 Min.)",
    practiceBadge: "Übungsrunde",
    practiceDoneTitle: "Übung geschafft!",
    practiceDoneSub: "Super gemacht — jetzt weißt du genau, wie es funktioniert. Es wurden noch keine Daten gespeichert.",
    practiceStartReal: "Jetzt richtig starten",
    labBtn: "Labor & Ergebnisse",
    heroTitle: "Fingerabdruck-Studie",
    heroSub: "Dauert ca. 8 Minuten — ganz ohne Zeitdruck. Einfach den Finger auflegen, wenn es angezeigt wird.",
    namePh: "Teilnehmer-ID (z. B. P01, P02)",
    nameReq: "Bitte gib zuerst deinen Namen ein.",
    modeClean: "Standard-Test",
    modeDirty: "Schmutz-Test",
    gameToggle: "Spiel-Modus (optionaler Timing-Ring)",
    begin: "Los geht’s",
    sensorElan: "Power-Taste",
    sensorFt: "USB-Sensor",
    sensorElanPhrase: "der Power-Taste",
    sensorFtPhrase: "dem USB-Sensor",
    sensorElanAcc: "die Power-Taste",
    sensorFtAcc: "den USB-Sensor",
    finger1: "Finger 1",
    finger2: "Finger 2",
    approveTitle: "Kurz bestätigen",
    approveSub: "Bitte das Passwort-Fenster einmal bestätigen, damit wir starten können.",
    halfwayTitle: "Halbzeit!",
    halfwaySub: (s) => `Jetzt wechseln wir zu ${s} — nimm genau dieselben zwei Finger wie eben.`,
    replayingTitle: "Ergebnisse werden berechnet…",
    replayingSub: "Alle 6 Algorithmen werden auf deinen gespeicherten Aufnahmen verglichen.",
    prepTitle: "Einen kurzen Moment…",
    prepSub: "Bitte kurz keinen Finger auflegen.",
    calTitle: "Finger kurz anheben",
    calSub: (s) => `Nimm den Finger kurz von ${s}, damit wir sauber starten können.`,
    cheers: ["Super! Kurz anheben", "Perfekt — Finger anheben", "Klasse! Kurz anheben", "Runde geschafft! Anheben"],
    liftSub: "Gleich geht es entspannt weiter.",
    f1FlatTitle: "Ersten Finger auflegen",
    f1FlatSub: (s) => `Nimm einfach deinen bequemsten Finger, lege ihn sanft auf ${s} und hebe ihn kurz an.`,
    f2SwapTitle: "Jetzt Finger wechseln!",
    f2SwapSub: (s) => `Super gemacht! Nimm jetzt einen zweiten, anderen Finger für ${s}.`,
    f2FlatTitle: "Zweiten Finger auflegen",
    f2FlatSub: (s) => `Lege deinen zweiten Finger sanft auf ${s} und hebe ihn kurz an.`,
    shiftTitle: "Leicht versetzen",
    shiftSub: (s) => `Gleicher Finger — diesmal ein kleines Stück neben der Mitte auf ${s}.`,
    tiltTitle: "Leicht kippen",
    tiltSub: (s) => `Gleicher Finger — sanft etwas nach links oder rechts neigen.`,
    cleanTestTitle: "Ganz normal auflegen",
    cleanTestSub: (s) => `Gleicher Finger — so entspannt auflegen wie beim normalen Entsperren.`,
    oilTitle: "Öl- / Creme-Test",
    oilSub: (s) => `Etwas Handcreme auf den Finger geben, dann ganz normal auf ${s} legen.`,
    grimeTitle: "Staub- / Schmutz-Test",
    grimeSub: (s) => `Finger leicht mit etwas Staub oder Mehl benetzen und auf ${s} legen.`,
    timingPerfect: (streak) => `Volltreffer! Perfektes Timing · ${streak}× Serie`,
    timingGood: "Schönes ruhiges Tempo!",
    doneTitle: (who) => who ? `Vielen Dank, ${who}!` : "Vielen Dank!",
    doneSub: "Alles fertig — deine Aufnahmen wurden sicher lokal auf diesem Computer gespeichert.",
    doneReplaySub: "Alle 6 Algorithmen wurden ausgewertet — öffne Labor & Ergebnisse für die Tabellen.",
    nextPerson: "Nächste Person",
    openLab: "Labor & Ergebnisse öffnen",
    discardRun: "Diese Aufnahme verwerfen",
    deleteBtn: "Löschen",
    confirmDelete: (who) => `Profil „${who}“ und alle gespeicherten Aufnahmen wirklich löschen?`,
    deletedHint: (who) => `Profil „${who}“ wurde gelöscht.`,
    pausedHint: "Aufnahme sauber beendet.",
    errHint: "Aufnahme unterbrochen — einfach erneut auf Los geht’s klicken.",
    labTitle: "Labor & Studienergebnisse",
    labSub: "Lokale Profile verwalten, alle 6 Algorithmen vergleichen und Diagramme ansehen.",
    labDone: "Fertig",
    labProfiles: "Gespeicherte Profile",
    labReplay: "Alle 6 Algorithmen auswerten",
    labPub: "Öffentliche Referenz-Tabelle (11 Datenbanken · 1.216.800 Tests)",
    labFigs: "Diagramme für das Paper (300 DPI — Klick zum Vergrößern)",
    labStress: "Live-Stresstests (fprintd)",
    noProfiles: "Noch keine Profile gespeichert.",
    optBoth: "Beide Sensoren (Standard)",
    optElan: "Nur Power-Taste (ELAN)",
    optFt: "Nur USB-Sensor (FT9201)",
    stressUlt: "Alle 3 Phasen",
    stressElan: "Nur Power-Taste",
    stressFt: "Nur USB-Sensor",
    stressDual: "Dual-Rennen",
    dirtyTag: "Schmutz",
    lidLabel: "Deckel",
    lidOpen: "offen",
    lidClosed: "geschlossen",
    colSensor: "Sensor",
    colAlgo: "Algorithmus",
    colTar: "Erkennung (TAR)",
    colFar: "Fehl-Akzeptanz (FAR)",
    colLive: "Live-Scan",
    colGen: "echt",
    colImp: "fremd",
    figCaps: {
      "fig1_algorithm_evolution.png": "Abb. 1 · Entwicklung über alle 6 Algorithmen",
      "fig2_security_det_curve.png":  "Abb. 2 · Sicherheit vs. Komfort (FAR/TAR)",
      "fig3_sensor_geometry.png":     "Abb. 3 · Sensor-Geometrie (64×80 vs. 150×52 vs. 96×96)",
      "fig4_speed_pareto.png":        "Abb. 4 · Geschwindigkeit & Pareto-Optimum",
    },
  },
  en: {
    pageTitle: "Fingerprint Study",
    stop: "Stop",
    stopping: "Stopping…",
    redoBtn: "↶ Redo Last",
    redoing: "Stepping back…",
    redoTitle: "Repeat Last Step",
    redoSub: "Last touch discarded — let’s calmly repeat that step.",
    practiceBtn: "Practice First (~1 min)",
    practiceBadge: "Practice",
    practiceDoneTitle: "Practice Complete!",
    practiceDoneSub: "Great job — now you know exactly how everything works. No data was saved yet.",
    practiceStartReal: "Start Real Run",
    labBtn: "Lab & Results",
    heroTitle: "Fingerprint Study",
    heroSub: "Takes about 8 minutes — zero rush. Just rest and lift your finger when shown.",
    namePh: "Participant ID (e.g. P01, P02)",
    nameReq: "Please enter your name first.",
    modeClean: "Standard Test",
    modeDirty: "Dirty / Grime Test",
    gameToggle: "Game Mode (optional timing ring)",
    begin: "Begin",
    sensorElan: "Power Button",
    sensorFt: "USB Reader",
    sensorElanPhrase: "the power button",
    sensorFtPhrase: "the USB reader",
    sensorElanAcc: "the power button",
    sensorFtAcc: "the USB reader",
    finger1: "Finger 1",
    finger2: "Finger 2",
    approveTitle: "Quick Confirmation",
    approveSub: "Approve the system password prompt once so we can begin.",
    halfwayTitle: "Halfway There!",
    halfwaySub: (s) => `Now switch to ${s} — use the exact same two fingers.`,
    replayingTitle: "Analyzing Results…",
    replayingSub: "Scoring all 6 algorithms on your saved raw scans.",
    prepTitle: "Getting Ready…",
    prepSub: "Keep your hands off both sensors for just a moment.",
    calTitle: "Lift Your Finger",
    calSub: (s) => `Lift your finger off ${s} briefly so we can calibrate cleanly.`,
    cheers: ["Nice! Lift your finger", "Got it — lift up", "Great! Lift your finger", "Set done! Lift up"],
    liftSub: "Take your time — ready for the next touch.",
    f1FlatTitle: "Rest Any Finger",
    f1FlatSub: (s) => `Pick whichever finger feels most comfortable, rest gently on ${s}, then lift.`,
    f2SwapTitle: "Switch to a Second Finger!",
    f2SwapSub: (s) => `Awesome! Now switch to a different finger on ${s}.`,
    f2FlatTitle: "Rest Your Second Finger",
    f2FlatSub: (s) => `Rest your second finger gently on ${s}, then lift.`,
    shiftTitle: "Shift Slightly",
    shiftSub: (s) => `Same finger — move a tiny bit off-center on ${s}.`,
    tiltTitle: "Tilt Slightly",
    tiltSub: (s) => `Same finger — gently rock slightly left or right.`,
    cleanTestTitle: "Everyday Touch",
    cleanTestSub: (s) => `Same finger — tap ${s} just like unlocking normally.`,
    oilTitle: "Oil / Lotion Touch",
    oilSub: (s) => `Apply light lotion to your finger, then tap ${s}.`,
    grimeTitle: "Dust / Grime Touch",
    grimeSub: (s) => `Lightly dab dust or flour on your finger, then tap ${s}.`,
    timingPerfect: (streak) => `Bullseye! Great rhythm · ${streak}× streak`,
    timingGood: "Nice steady rhythm!",
    doneTitle: (who) => who ? `Thank you, ${who}!` : "Thank you!",
    doneSub: "All done — your scans are saved safely on this computer.",
    doneReplaySub: "All 6 algorithms have been scored — open Lab & Results to view the tables.",
    nextPerson: "Next Person",
    openLab: "Open Lab & Results",
    discardRun: "Discard This Run",
    deleteBtn: "Delete",
    confirmDelete: (who) => `Delete profile "${who}" and all its saved scans?`,
    deletedHint: (who) => `Deleted profile "${who}".`,
    pausedHint: "Session stopped cleanly.",
    errHint: "Capture interrupted — click Begin whenever you're ready.",
    labTitle: "Lab & Study Results",
    labSub: "Manage local profiles, replay all 6 algorithms, and inspect publication charts.",
    labDone: "Done",
    labProfiles: "Saved Profiles",
    labReplay: "Run 6-Algorithm Replay",
    labPub: "Public Corpus Master Table (11 DBs · 1,216,800 Trials)",
    labFigs: "Publication Figures (300 DPI — Click to Enlarge)",
    labStress: "Live fprintd Stress Tests",
    noProfiles: "No profiles saved yet.",
    optBoth: "Both Sensors (Default)",
    optElan: "Power Button Only (ELAN)",
    optFt: "USB Reader Only (FT9201)",
    stressUlt: "All 3 Phases",
    stressElan: "Power Button Only",
    stressFt: "USB Reader Only",
    stressDual: "Dual Race",
    dirtyTag: "Dirty",
    lidLabel: "Lid",
    lidOpen: "open",
    lidClosed: "closed",
    colSensor: "Sensor",
    colAlgo: "Algorithm",
    colTar: "Match Rate (TAR)",
    colFar: "False Accept (FAR)",
    colLive: "Live Scan",
    colGen: "genuine",
    colImp: "impostor",
    figCaps: {
      "fig1_algorithm_evolution.png": "Fig. 1 · Evolution Across All 6 Algorithms",
      "fig2_security_det_curve.png":  "Fig. 2 · Security vs. Convenience (FAR/TAR)",
      "fig3_sensor_geometry.png":     "Fig. 3 · Sensor Geometry (64×80 vs. 150×52 vs. 96×96)",
      "fig4_speed_pareto.png":        "Fig. 4 · Speed & Pareto Frontier",
    },
  },
};

let lang = "de"; // Default to German for family & friends!
let selectedMode = "clean";
let gameMode = false;
let activeParticipant = "";
let showingDone = false;
let lastDoneCount = -1;
let lastFingerNum = 1;
let lastPhase = "idle";
let swapUntilMs = 0;
let swapTimerId = null;
let lastState = null;

// Optional playful timing tracker (only active when Spiel-Modus is toggled ON!)
let timingPhaseStart = performance.now();
let timingStreak = 0;

const shellEl = document.querySelector("main.shell");
const viewWelcome = document.getElementById("view-welcome");
const viewTask = document.getElementById("view-task");
const viewDone = document.getElementById("view-done");

const btnStopTop = document.getElementById("btn-stop-top");
const btnRedoTop = document.getElementById("btn-redo-top");
const btnOpenLab = document.getElementById("btn-open-lab");
const btnLang = document.getElementById("btn-lang");
const btnPractice = document.getElementById("btn-practice");
const stepPill = document.getElementById("step-pill");
const streakPill = document.getElementById("streak-pill");
let lastStudyMode = "clean";
let lastRedoSeq = 0;

const nameInput = document.getElementById("name-input");
const welcomeForm = document.getElementById("welcome-form");
const welcomeHint = document.getElementById("welcome-status-hint");
const chkGameMode = document.getElementById("chk-game-mode");

const sensorCueText = document.getElementById("sensor-cue-text");
const fingerIndText = document.getElementById("finger-ind-text");
const taskLensWrap = document.getElementById("task-lens-wrap");
const progressRing = document.getElementById("progress-ring");
const dotRow = document.getElementById("dot-row");
const taskTitle = document.getElementById("task-title");
const taskSub = document.getElementById("task-sub");
const timingCheer = document.getElementById("timing-cheer");

const doneTitle = document.getElementById("done-title");
const doneSub = document.getElementById("done-sub");

const labBackdrop = document.getElementById("lab-backdrop");
const labSensorSelect = document.getElementById("lab-sensor-select");

const RING_CIRC = 2 * Math.PI * 124; // 779.11

function t() { return I18N[lang]; }
function capitalize(s) { return s ? s.charAt(0).toUpperCase() + s.slice(1) : ""; }
function escHtml(s) {
  return String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#39;" }[c]));
}

function applyStaticLabels() {
  const L = t();
  document.documentElement.lang = lang;
  document.title = L.pageTitle;
  btnLang.textContent = lang.toUpperCase();
  btnStopTop.textContent = L.stop;
  btnRedoTop.textContent = L.redoBtn;
  btnOpenLab.textContent = L.labBtn;
  document.getElementById("txt-hero-title").textContent = L.heroTitle;
  document.getElementById("txt-hero-sub").textContent = L.heroSub;
  nameInput.placeholder = L.namePh;
  document.getElementById("pill-mode-clean").textContent = L.modeClean;
  document.getElementById("pill-mode-dirty").textContent = L.modeDirty;
  document.getElementById("txt-game-toggle").textContent = L.gameToggle;
  document.getElementById("btn-begin").textContent = L.begin;
  btnPractice.textContent = L.practiceBtn;
  document.getElementById("btn-next-person").textContent = lastStudyMode === "practice" ? L.practiceStartReal : L.nextPerson;
  document.getElementById("btn-done-results").textContent = L.openLab;
  document.getElementById("btn-done-discard").textContent = L.discardRun;
  document.getElementById("txt-lab-title").textContent = L.labTitle;
  document.getElementById("txt-lab-sub").textContent = L.labSub;
  document.getElementById("btn-close-lab").textContent = L.labDone;
  document.getElementById("txt-lab-profiles").textContent = L.labProfiles;
  document.getElementById("btn-lab-replay").textContent = L.labReplay;
  document.getElementById("txt-lab-pub").textContent = L.labPub;
  document.getElementById("txt-lab-figs").textContent = L.labFigs;
  document.getElementById("txt-lab-stress").textContent = L.labStress;
  const opts = labSensorSelect.options;
  if (opts.length >= 3) {
    opts[0].textContent = L.optBoth;
    opts[1].textContent = L.optElan;
    opts[2].textContent = L.optFt;
  }
  document.getElementById("btn-stress-ultimate").textContent = L.stressUlt;
  document.getElementById("btn-stress-elan").textContent = L.stressElan;
  document.getElementById("btn-stress-ft").textContent = L.stressFt;
  document.getElementById("btn-stress-dual").textContent = L.stressDual;
  if (!labBackdrop.hidden) refreshLabData();
  if (lastState) render(lastState);
}

function setView(view) {
  shellEl.dataset.view = view;
  viewWelcome.hidden = view !== "welcome";
  viewTask.hidden = view !== "task";
  viewDone.hidden = view !== "done";

  const inTask = view === "task";
  btnStopTop.hidden = !inTask;
  btnRedoTop.hidden = !inTask;
  btnOpenLab.hidden = inTask;
  if (!inTask) {
    stepPill.hidden = true;
    streakPill.hidden = true;
  }
}

function updateDots(curTouch, totalSet, justCaptured) {
  const dots = dotRow.querySelectorAll(".touch-dot");
  if (!totalSet || totalSet < 1 || totalSet > 4) {
    dotRow.classList.add("invisible");
    return;
  }
  dotRow.classList.remove("invisible");
  dots.forEach((d, idx) => {
    const n = idx + 1;
    d.hidden = n > totalSet;
    const filled = justCaptured ? (n <= curTouch) : (n < curTouch);
    const current = !justCaptured && (n === curTouch);
    d.classList.toggle("filled", filled);
    d.classList.toggle("current", current);
  });
}

function humanizeTask(s, fingerNum, inSwapBanner) {
  const L = t();
  const rawAction = s.action || "";
  const rawInstr = s.instruction || "";
  const phase = s.phase || "idle";
  const sensor = s.sensor === "ft9201" ? "ft9201" : "elan";
  const sPhrase = sensor === "ft9201" ? L.sensorFtPhrase : L.sensorElanPhrase;
  const sAcc = sensor === "ft9201" ? L.sensorFtAcc : L.sensorElanAcc;
  const study = s.study || {};

  let curTouch = 0, totalSet = 4;
  const m = rawAction.match(/\((\d+)\/(\d+)\)/);
  if (m) { curTouch = parseInt(m[1], 10); totalSet = parseInt(m[2], 10); }

  if (inSwapBanner) {
    return { title: L.f2SwapTitle, sub: L.f2SwapSub(sAcc), curTouch: 1, totalSet: 4, visualPhase: "swap" };
  }

  if (phase === "prepare") {
    if (rawAction.includes("Approve System Prompt")) return { title: L.approveTitle, sub: L.approveSub, curTouch: 0, totalSet: 0 };
    if (rawAction.includes("Switch to")) return { title: L.halfwayTitle, sub: L.halfwaySub(sPhrase), curTouch: 0, totalSet: 0, visualPhase: "sensor-swap" };
    if (rawAction.includes("Redo Last Step")) return { title: L.redoTitle, sub: L.redoSub, curTouch: 0, totalSet: 0 };
    if (rawAction.includes("Replaying")) return { title: L.replayingTitle, sub: L.replayingSub, curTouch: 0, totalSet: 0 };
    return { title: L.prepTitle, sub: L.prepSub, curTouch: 0, totalSet: 0 };
  }

  if (phase === "lift") {
    if (rawAction.includes("Lift Your Finger") && rawInstr.includes("calibrate")) {
      return { title: L.calTitle, sub: L.calSub(sPhrase), curTouch: 0, totalSet: 0 };
    }
    const doneInSet = curTouch > 0 ? curTouch : (((study.doneTouches || 1) - 1) % totalSet + 1);
    const msg = doneInSet === totalSet ? L.cheers[3] : L.cheers[(study.doneTouches || 0) % 3];
    return { title: msg, sub: L.liftSub, curTouch: doneInSet, totalSet, justCaptured: true };
  }

  if (rawAction.includes("Finger 1 · Natural Flat")) return { title: L.f1FlatTitle, sub: L.f1FlatSub(sAcc), curTouch, totalSet };
  if (rawAction.includes("Finger 2 · Natural Flat")) return { title: L.f2FlatTitle, sub: L.f2FlatSub(sAcc), curTouch, totalSet };
  if (rawAction.includes("Slight Shifts") || rawAction.includes("Shifted Test")) return { title: L.shiftTitle, sub: L.shiftSub(sPhrase), curTouch, totalSet };
  if (rawAction.includes("Gentle Tilts") || rawAction.includes("Tilted Test")) return { title: L.tiltTitle, sub: L.tiltSub(sPhrase), curTouch, totalSet };
  if (rawAction.includes("Clean Test")) return { title: L.cleanTestTitle, sub: L.cleanTestSub(sPhrase), curTouch, totalSet };
  if (rawAction.includes("Oily")) return { title: L.oilTitle, sub: L.oilSub(sAcc), curTouch, totalSet };
  if (rawAction.includes("Grime")) return { title: L.grimeTitle, sub: L.grimeSub(sAcc), curTouch, totalSet };

  return { title: rawAction.replace(/\s*\(\d+\/\d+\)/, "") || L.f1FlatTitle, sub: rawInstr || L.f1FlatSub(sPhrase), curTouch, totalSet };
}

function render(s) {
  lastState = s;
  const L = t();
  const study = s.study || {};
  const running = !!s.running;
  const phase = s.phase || "idle";
  const sensor = s.sensor === "ft9201" ? "ft9201" : "elan";
  const fingerNum = ((study.fingerLabel || s.action || "").includes("Finger 2")) ? 2 : 1;

  shellEl.dataset.sensor = sensor;
  shellEl.dataset.motion = s.motion || "flat";
  shellEl.dataset.fingerNum = String(fingerNum);
  shellEl.dataset.game = gameMode ? "on" : "off";

  const lidNice = (s.lidState || "open") === "closed" ? L.lidClosed : L.lidOpen;
  document.getElementById("lab-lid-text").textContent = `${L.lidLabel}: ${lidNice}`;
  document.getElementById("btn-lab-replay").disabled = running;

  if (running) {
    showingDone = false;
    labBackdrop.hidden = true;
    setView("task");

    if (study.mode) lastStudyMode = study.mode;
    if (study.profile && study.mode !== "practice") activeParticipant = study.profile;

    // Detect Finger 1 -> Finger 2 switch and trigger unmistakable 3.5-second Finger-Swap animation!
    if (fingerNum === 2 && lastFingerNum === 1 && phase === "touch") {
      swapUntilMs = Date.now() + 3500;
      if (swapTimerId) clearTimeout(swapTimerId);
      swapTimerId = setTimeout(() => { if (lastState) render(lastState); }, 3550);
    }
    if (phase === "lift" || fingerNum === 1) {
      swapUntilMs = 0; // Dismiss swap animation immediately if user already touched Finger 2!
    }
    lastFingerNum = fingerNum;
    const inSwapBanner = fingerNum === 2 && phase === "touch" && Date.now() < swapUntilMs;

    sensorCueText.textContent = sensor === "ft9201" ? L.sensorFt : L.sensorElan;
    fingerIndText.textContent = fingerNum === 2 ? L.finger2 : L.finger1;

    if (study.active) {
      const tot = study.totalTouches || 1;
      const done = study.doneTouches || 0;
      const pct = Math.min(1, done / tot);
      progressRing.style.strokeDashoffset = String(RING_CIRC * (1 - pct));
      stepPill.hidden = false;
      const whoBadge = study.mode === "practice" ? L.practiceBadge : capitalize(study.profile || activeParticipant);
      stepPill.textContent = `${whoBadge} · ${Math.round(pct * 100)}%`;

      // Update Redo Last button availability
      btnRedoTop.hidden = false;
      const redoBusy = phase === "prepare" && (s.action || "").includes("Redo");
      btnRedoTop.disabled = done <= 0 || redoBusy;
      btnRedoTop.textContent = redoBusy ? L.redoing : L.redoBtn;

      const rSeq = study.redoSeq || 0;
      if (rSeq > lastRedoSeq) {
        lastRedoSeq = rSeq;
        timingStreak = Math.max(0, timingStreak - 1);
        streakPill.hidden = timingStreak < 2;
        timingCheer.textContent = "";
      }

      // Optional Spiel-Modus rhythm evaluation on fresh touch capture
      if (done > lastDoneCount && lastDoneCount >= 0) {
        taskLensWrap.classList.remove("pop");
        void taskLensWrap.offsetWidth;
        taskLensWrap.classList.add("pop");
        setTimeout(() => taskLensWrap.classList.remove("pop"), 240);

        if (gameMode) {
          const beatPos = ((performance.now() - timingPhaseStart) % 2000) / 2000; // 0.32..0.78 = sweet spot
          if (beatPos >= 0.32 && beatPos <= 0.78) {
            timingStreak++;
            timingCheer.textContent = L.timingPerfect(timingStreak);
          } else {
            timingStreak = Math.max(0, timingStreak);
            timingCheer.textContent = L.timingGood;
          }
          streakPill.hidden = timingStreak < 2;
          streakPill.textContent = `🔥 ${timingStreak}×`;
        } else {
          timingCheer.textContent = "";
          streakPill.hidden = true;
        }
      } else if (phase === "touch" && lastPhase !== "touch") {
        timingPhaseStart = performance.now();
        if (!gameMode) timingCheer.textContent = "";
      }
      lastDoneCount = done;
    } else {
      btnRedoTop.hidden = true;
      stepPill.hidden = !s.progress;
      stepPill.textContent = s.progress || "";
      const p = s.enrollPct != null ? s.enrollPct / 100 : 0;
      progressRing.style.strokeDashoffset = String(RING_CIRC * (1 - p));
      timingCheer.textContent = "";
      streakPill.hidden = true;
    }

    lastPhase = phase;
    const copy = humanizeTask(s, fingerNum, inSwapBanner);
    shellEl.dataset.phase = copy.visualPhase || phase;
    taskTitle.textContent = copy.title;
    taskSub.textContent = copy.sub;
    updateDots(copy.curTouch, copy.totalSet, !!copy.justCaptured);
    return;
  }

  // Not running
  btnStopTop.disabled = false;
  btnStopTop.textContent = L.stop;
  btnRedoTop.disabled = true;
  btnRedoTop.textContent = L.redoBtn;
  lastDoneCount = -1;
  lastFingerNum = 1;
  lastRedoSeq = 0;
  lastPhase = phase;
  timingStreak = 0;
  timingCheer.textContent = "";
  shellEl.dataset.phase = phase;

  if (phase === "complete" && !showingDone) {
    showingDone = true;
    const isPractice = (study.mode || lastStudyMode) === "practice";
    const isReplay = !!(s.action && s.action.includes("Replay"));
    const nextBtn = document.getElementById("btn-next-person");
    const discardBtn = document.getElementById("btn-done-discard");
    if (isPractice) {
      doneTitle.textContent = L.practiceDoneTitle;
      doneSub.textContent = L.practiceDoneSub;
      nextBtn.textContent = L.practiceStartReal;
      discardBtn.hidden = true;
    } else {
      const who = capitalize(activeParticipant);
      doneTitle.textContent = L.doneTitle(who);
      doneSub.textContent = isReplay ? L.doneReplaySub : L.doneSub;
      nextBtn.textContent = L.nextPerson;
      discardBtn.hidden = isReplay || !activeParticipant;
    }
    setView("done");
    refreshLabData();
  } else if (phase === "error") {
    showingDone = false;
    setView("welcome");
    welcomeHint.textContent = L.errHint;
  } else if (phase === "stopped" || phase === "idle") {
    if (!showingDone) {
      setView("welcome");
      if (phase === "stopped") welcomeHint.textContent = L.pausedHint;
    }
  }
}

/* === Controls & Events === */
btnLang.addEventListener("click", () => {
  lang = lang === "de" ? "en" : "de";
  applyStaticLabels();
});

for (const btn of document.querySelectorAll(".seg-pill")) {
  btn.addEventListener("click", () => {
    selectedMode = btn.dataset.mode;
    for (const x of document.querySelectorAll(".seg-pill")) x.classList.toggle("active", x === btn);
  });
}

chkGameMode.addEventListener("change", () => {
  gameMode = chkGameMode.checked;
  shellEl.dataset.game = gameMode ? "on" : "off";
});

welcomeForm.addEventListener("submit", async (e) => {
  e.preventDefault();
  const raw = nameInput.value.trim();
  if (!raw) {
    nameInput.focus();
    welcomeHint.textContent = t().nameReq;
    return;
  }
  activeParticipant = raw;
  welcomeHint.textContent = "";
  const sensors = labSensorSelect.value || "elan,ft9201";
  const r = await post("/study/start", { profile: raw, mode: selectedMode, sensors });
  if (r && r.ok === false && r.message) welcomeHint.textContent = r.message;
});

btnPractice.addEventListener("click", async () => {
  welcomeHint.textContent = "";
  lastStudyMode = "practice";
  const raw = nameInput.value.trim();
  const sensors = labSensorSelect.value || "elan,ft9201";
  const r = await post("/study/start", { profile: raw || "practice", mode: "practice", sensors });
  if (r && r.ok === false && r.message) welcomeHint.textContent = r.message;
});

btnStopTop.addEventListener("click", async () => {
  btnStopTop.disabled = true;
  btnStopTop.textContent = t().stopping;
  await post("/stop");
});

async function triggerRedo() {
  if (btnRedoTop.hidden || btnRedoTop.disabled) return;
  btnRedoTop.disabled = true;
  btnRedoTop.textContent = t().redoing;
  await post("/study/redo");
}
btnRedoTop.addEventListener("click", triggerRedo);
window.addEventListener("keydown", (e) => {
  if (shellEl.dataset.view !== "task") return;
  if (e.target && (e.target.tagName === "INPUT" || e.target.tagName === "TEXTAREA")) return;
  if (e.key === "Backspace") {
    e.preventDefault();
    triggerRedo();
  }
});

document.getElementById("btn-done-discard").addEventListener("click", async () => {
  if (!activeParticipant) return;
  const clean = activeParticipant.trim().toLowerCase().replace(/[^a-z0-9_-]/g, "");
  if (!clean) return;
  if (!window.confirm(t().confirmDelete(clean))) return;
  const r = await post("/profiles/delete", { profile: clean });
  if (r && r.ok) {
    showingDone = false;
    activeParticipant = "";
    nameInput.value = "";
    welcomeHint.textContent = t().deletedHint(clean);
    setView("welcome");
    refreshLabData();
  }
});

document.getElementById("btn-next-person").addEventListener("click", () => {
  const wasPractice = lastStudyMode === "practice";
  showingDone = false;
  lastStudyMode = selectedMode;
  if (!wasPractice) {
    activeParticipant = "";
    nameInput.value = "";
  }
  welcomeHint.textContent = "";
  setView("welcome");
  setTimeout(() => nameInput.focus(), 50);
});

/* === Researcher Lab Drawer === */
function openLab() { labBackdrop.hidden = false; refreshLabData(); }
function closeLab() { labBackdrop.hidden = true; }
btnOpenLab.addEventListener("click", openLab);
document.getElementById("btn-done-results").addEventListener("click", openLab);
document.getElementById("btn-close-lab").addEventListener("click", closeLab);
labBackdrop.addEventListener("click", (e) => { if (e.target === labBackdrop) closeLab(); });

document.getElementById("btn-lab-replay").addEventListener("click", () => { closeLab(); showingDone = false; post("/study/replay"); });
document.getElementById("btn-stress-ultimate").addEventListener("click", () => { closeLab(); post("/start/ultimate"); });
document.getElementById("btn-stress-elan").addEventListener("click", () => { closeLab(); post("/start/elan"); });
document.getElementById("btn-stress-ft").addEventListener("click", () => { closeLab(); post("/start/ft9201"); });
document.getElementById("btn-stress-dual").addEventListener("click", () => { closeLab(); post("/start/dual"); });

function fmtPct(v) { return v == null ? "—" : `${(v * 100).toFixed(1)}%`; }

async function refreshLabData() {
  try {
    const [pRes, sRes] = await Promise.all([fetch("/profiles"), fetch("/paper-summary")]);
    const pData = await pRes.json();
    const sData = await sRes.json();

    const L = t();
    const plist = document.getElementById("lab-profiles-list");
    const profiles = pData.profiles || [];
    if (!profiles.length) {
      plist.innerHTML = `<p class="muted-small">${L.noProfiles}</p>`;
    } else {
      plist.innerHTML = profiles.map(p => {
        const tags = [];
        if (p.elanClean) tags.push(`<span class="ptag">${L.sensorElan}: ${p.elanClean}</span>`);
        if (p.ftClean) tags.push(`<span class="ptag ft">${L.sensorFt}: ${p.ftClean}</span>`);
        if (p.elanDirty || p.ftDirty) tags.push(`<span class="ptag dirty">${L.dirtyTag}: ${p.elanDirty + p.ftDirty}</span>`);
        const safeName = escHtml(p.name);
        return `<div class="prof-row"><span class="prof-name">${safeName}</span><div class="prof-actions"><div class="prof-tags">${tags.join("")}</div><button class="prof-del-btn" type="button" data-del-profile="${safeName}">${L.deleteBtn}</button></div></div>`;
      }).join("");
      for (const b of plist.querySelectorAll("[data-del-profile]")) {
        b.addEventListener("click", async () => {
          const pname = b.dataset.delProfile;
          if (!window.confirm(t().confirmDelete(pname))) return;
          const r = await post("/profiles/delete", { profile: pname });
          if (r && r.ok) {
            refreshLabData();
          } else if (r && r.message) {
            window.alert(r.message);
          }
        });
      }
    }

    const liveWrap = document.getElementById("lab-live-replay-wrap");
    if (sData.live && Object.keys(sData.live).length > 0) {
      let html = "";
      for (const [, entry] of Object.entries(sData.live)) {
        html += `<h4 style="margin:.9rem 0 .35rem;font-size:.82rem;color:#0071E3;">${entry.sensor} · ${entry.condition} (${entry.profiles.join(", ")} — ${entry.n_gen} ${L.colGen} / ${entry.n_imp} ${L.colImp})</h4>`;
        html += `<div class="table-wrap"><table class="paper-table"><thead><tr><th>${L.colAlgo}</th><th>${L.colTar}</th><th>${L.colFar}</th><th>CPU</th></tr></thead><tbody>`;
        for (const r of entry.results) {
          const tar = `${(100 * r.gen_ok / Math.max(entry.n_gen, 1)).toFixed(1)}% (${r.gen_ok}/${entry.n_gen})`;
          const far = entry.n_imp > 0 ? `${(100 * r.imp_fa / entry.n_imp).toFixed(2)}% (${r.imp_fa}/${entry.n_imp})` : "—";
          const lat = r.lat_ms != null ? `${r.lat_ms.toFixed(1)} ms` : "14–17 ms";
          html += `<tr class="${r.algo.includes("v4.1") ? "hl-v41" : ""}"><td>${r.algo}</td><td>${tar}</td><td>${far}</td><td>${lat}</td></tr>`;
        }
        html += `</tbody></table></div>`;
      }
      liveWrap.innerHTML = html;
    } else {
      liveWrap.innerHTML = "";
    }

    const pubWrap = document.getElementById("lab-public-table");
    if (sData.public) {
      const algos = [
        ["nbis", "1. NBIS (mindtct + bozorth3)"],
        ["v0",   "2. Upstream v0 (Lowe SIFT)"],
        ["v1",   "3. PressMatch v1 (RootSIFT)"],
        ["v2",   "4. PressMatch v2 (PoseGraph)"],
        ["v40",  "5. PressMatch v4.0 (Rigid ZNCC)"],
        ["v41",  "6. PressMatch v4.1 (Elastic ZNCC ★)"],
      ];
      let html = `<table class="paper-table"><thead><tr><th>${L.colSensor}</th><th>${L.colAlgo}</th><th>TAR ≤1/50k</th><th>${L.colLive}</th><th>${L.colFar}</th><th>CPU</th></tr></thead><tbody>`;
      for (const gkey of ["64x80", "150x52", "96x96"]) {
        const gd = sData.public[gkey];
        if (!gd) continue;
        for (const [ak, aname] of algos) {
          const ad = gd[ak];
          if (!ad) continue;
          const sh = ad.shipped || {};
          html += `<tr class="${ak === "v41" ? "hl-v41" : ""}">
            <td>${gd.label}</td><td>${aname}</td>
            <td>${fmtPct(ad.at_50k?.tar_all)}</td>
            <td>${fmtPct(ad.at_50k?.tar_live)}</td>
            <td>${sh.fa_all ?? 0} / ${(sh.imp_all_n || 405600).toLocaleString()}</td>
            <td>${ad.lat_p50.toFixed(1)} ms</td>
          </tr>`;
        }
      }
      html += `</tbody></table>`;
      pubWrap.innerHTML = html;
    }

    const figGrid = document.getElementById("lab-figures-grid");
    const caps = L.figCaps;
    figGrid.innerHTML = (sData.figures || []).map(f =>
      `<a class="fig-card" href="/figures/${f}" target="_blank" rel="noopener">
        <img src="/figures/${f}" alt="${caps[f] || f}">
        <div class="fig-cap">${caps[f] || f}</div>
      </a>`
    ).join("");
  } catch (_) {}
}

async function post(url, body = null) {
  const opts = { method: "POST" };
  if (body) {
    opts.headers = { "Content-Type": "application/json" };
    opts.body = JSON.stringify(body);
  }
  const r = await fetch(url, opts);
  return r.json().catch(() => ({}));
}

function connect() {
  const es = new EventSource("/events");
  es.onmessage = e => render(JSON.parse(e.data));
}

applyStaticLabels();
fetch("/state").then(r => r.json()).then(render).catch(() => {});
connect();
