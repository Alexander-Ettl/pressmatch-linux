/*
 * PressMatchPill.qml — OLED G3 Corner Fingerprint HUD for KDE Plasma 6 Lockscreen
 *
 * Visual twin of pressmatch_hud.py:
 *   - Pure-black (#000000) CAD G3 corner fillet flush to the bottom-right display edge
 *   - Outer-only Apple Dynamic Island specular keyline + soft OLED bloom (0% at bezel ends)
 *   - Mechanical under-damped spring extension & smooth retraction
 *   - Animated Touch ID vertical whorl glyph + curved status text along the inner fillet
 *   - Posture & pressure animations driven directly from PAM hints (too light, too hard,
 *     off-center, wrong area / micro-orbit, dry, declined, match)
 */
import QtQuick

Item {
    id: pillRoot

    // Public state API (preserved 100% for LockScreenUi.qml compatibility):
    //   "hidden" | "armed" | "scanning" | "match" | "retry" | "partial" | "declined"
    property string hudState: "hidden"
    property string activeDevice: "ELAN · FT9201"
    property string readersLabel: "ELAN · FT9201"
    property int retryCount: 0
    property int maxRetries: 5
    property string hintText: ""

    readonly property bool isDe: Qt.locale().name.toLowerCase().startsWith("de")
    readonly property real lMax: 360.0
    readonly property real bloomPad: 28.0

    width: lMax + bloomPad
    height: lMax + bloomPad
    visible: currentL > 0.5 || hudState !== "hidden"

    // Animation state
    property real currentL: 0.0
    property real driveFrom: 0.0
    property real driveTo: 0.0
    property real driveElapsed: 0.0
    property real driveDuration: 0.72
    property bool driving: false
    property real contentAlpha: 0.0

    property real phase: 0.0
    property real stateElapsed: 0.0
    property real glintElapsed: 99.0
    readonly property real glintDuration: 0.92
    property string hintKind: ""

    function classifyHint(hint, fallbackState) {
        const low = String(hint || "").toLowerCase();
        if (low.indexOf("firmer") !== -1 || low.indexOf("light") !== -1 || low.indexOf("cover") !== -1 || low.indexOf("fester") !== -1)
            return "too_light";
        if (low.indexOf("gently") !== -1 || low.indexOf("hard") !== -1 || low.indexOf("sanfter") !== -1)
            return "too_hard";
        if (low.indexOf("center") !== -1 || low.indexOf("zentrier") !== -1)
            return "off_center";
        if (low.indexOf("still") !== -1 || low.indexOf("moving") !== -1 || low.indexOf("ruhig") !== -1)
            return "moving";
        if (low.indexOf("wipe") !== -1 || low.indexOf("dry") !== -1 || low.indexOf("wisch") !== -1)
            return "dry";
        if (low.indexOf("setup") !== -1 || low.indexOf("move") !== -1 || low.indexOf("slightly") !== -1)
            return "wrong_area";
        if (fallbackState === "partial")
            return "too_light";
        return "";
    }

    function startDrive(target, dur) {
        driveFrom = currentL;
        driveTo = target;
        driveElapsed = 0.0;
        driveDuration = dur;
        driving = true;
    }

    function ensureExtended() {
        if (currentL < lMax * 0.5 && !(driving && driveTo >= lMax * 0.9)) {
            startDrive(lMax, 0.72);
        }
    }

    function setArmed(readersText) {
        if (readersText && readersText.length > 0) readersLabel = readersText;
        activeDevice = readersLabel;
        hudState = "armed";
        hintText = "";
        hintKind = "";
        stateElapsed = 0.0;
        retryRevertTimer.stop();
        ensureExtended();
        canvas.requestPaint();
    }

    function setScanning(devText) {
        if (devText && devText.length > 0) activeDevice = devText;
        hudState = "scanning";
        hintKind = "";
        stateElapsed = 0.0;
        retryRevertTimer.stop();
        ensureExtended();
        canvas.requestPaint();
    }

    function setMatch(devText) {
        if (devText && devText.length > 0) activeDevice = devText;
        hudState = "match";
        hintKind = "";
        stateElapsed = 0.0;
        glintElapsed = 0.0;
        retryRevertTimer.stop();
        ensureExtended();
        canvas.requestPaint();
    }

    function setPartial(devText) {
        if (hudState === "declined") return;
        if (devText && devText.length > 0) activeDevice = devText;
        hudState = "partial";
        hintKind = classifyHint(hintText, "partial");
        stateElapsed = 0.0;
        retryRevertTimer.interval = 3500;
        retryRevertTimer.restart();
        ensureExtended();
        canvas.requestPaint();
    }

    function setDeclined() {
        hudState = "declined";
        hintKind = "";
        stateElapsed = 0.0;
        retryRevertTimer.stop();
        ensureExtended();
        canvas.requestPaint();
    }

    function setRetry(devText) {
        retryCount = retryCount + 1;
        if (devText && devText.length > 0) activeDevice = devText;
        hudState = "retry";
        hintKind = classifyHint(hintText, "retry");
        stateElapsed = 0.0;
        retryRevertTimer.interval = hintText ? 3500 : 1400;
        retryRevertTimer.restart();
        ensureExtended();
        canvas.requestPaint();
    }

    function parsePamMessage(msg) {
        if (!msg) return false;
        const s = String(msg).trim();
        if (!s) return false;
        const low = s.toLowerCase();
        const dash = s.indexOf(" — ");
        const tail = dash !== -1 ? s.substring(dash + 3).trim() : "";
        const hint = tail.toLowerCase().indexOf("try again on either reader") !== -1 ? "" : tail;

        if (low.indexOf("scanning fingerprint") !== -1) {
            let dev = readersLabel;
            if (low.indexOf("elan") !== -1) dev = "ELAN";
            else if (low.indexOf("ft9201") !== -1 || low.indexOf("focal") !== -1) dev = "FT9201";
            setScanning(dev);
            return true;
        }
        if (low.indexOf("place your finger") !== -1) {
            let readers = "ELAN · FT9201";
            if (low.indexOf("elan") !== -1 && low.indexOf("ft9201") !== -1) {
                readers = "ELAN · FT9201";
            } else if (low.indexOf("ft9201") !== -1 || low.indexOf("focal") !== -1) {
                readers = low.indexOf("lid closed") !== -1 ? "FT9201 (Lid Closed)" : "FT9201";
            } else if (low.indexOf("elan") !== -1) {
                readers = "ELAN";
            }
            retryCount = 0;
            setArmed(readers);
            return true;
        }
        if (low.indexOf("declined") !== -1) {
            setDeclined();
            return true;
        }
        if (low.indexOf("touch not read") !== -1 || low.indexOf("cover the whole") !== -1) {
            hintText = hint;
            let dev = activeDevice;
            if (low.indexOf("elan") !== -1) dev = "ELAN";
            else if (low.indexOf("ft9201") !== -1) dev = "FT9201";
            setPartial(dev);
            return true;
        }
        if (low.indexOf("failed to match") !== -1 || low.indexOf("try again") !== -1) {
            hintText = hint;
            setRetry(activeDevice);
            return true;
        }
        return false;
    }

    readonly property string curveLabel: {
        if (hudState === "scanning") return isDe ? "Wird gelesen…" : "Reading…";
        if (hudState === "match") return isDe ? "Entsperrt" : "Unlocked";
        if (hudState === "declined") return isDe ? "Passwort verwenden" : "Use your password";
        if (hudState === "partial" || hudState === "retry") {
            if (hintKind === "too_light") return isDe ? "Fester drücken" : "Press a little firmer";
            if (hintKind === "too_hard") return isDe ? "Sanfter drücken" : "Press more gently";
            if (hintKind === "off_center") return isDe ? "Finger zentrieren" : "Center your finger";
            if (hintKind === "moving") return isDe ? "Ruhig halten" : "Hold still until it reacts";
            if (hintKind === "dry") return isDe ? "Finger & Sensor abwischen" : "Wipe finger and sensor";
            if (hintKind === "wrong_area") return isDe ? "Finger leicht bewegen" : "Move finger slightly";
            if (hintText) return hintText;
            const base = isDe ? "Nochmal versuchen" : "Try again";
            return retryCount > 0 ? (base + " (" + retryCount + "/" + maxRetries + ")") : base;
        }
        return isDe ? "Zum Entsperren berühren" : "Touch to unlock";
    }

    Timer {
        id: retryRevertTimer
        interval: 1400
        repeat: false
        onTriggered: {
            if (pillRoot.hudState === "retry" || pillRoot.hudState === "partial") {
                pillRoot.hudState = "armed";
                pillRoot.hintKind = "";
                pillRoot.hintText = "";
                pillRoot.activeDevice = pillRoot.readersLabel;
                pillRoot.stateElapsed = 0.0;
                canvas.requestPaint();
            }
        }
    }

    Timer {
        id: animTimer
        interval: 16
        repeat: true
        running: pillRoot.visible && (pillRoot.hudState !== "hidden" || pillRoot.driving)
        onTriggered: {
            pillRoot.phase += 0.016;
            pillRoot.stateElapsed += 0.016;
            if (pillRoot.glintElapsed <= pillRoot.glintDuration) {
                pillRoot.glintElapsed += 0.016;
            }
            if (pillRoot.driving) {
                pillRoot.driveElapsed += 0.016;
                const t = Math.min(1.0, pillRoot.driveElapsed / pillRoot.driveDuration);
                const extending = pillRoot.driveTo > pillRoot.driveFrom;
                let e;
                if (extending) {
                    if (t >= 1.0) {
                        e = 1.0;
                    } else {
                        const w = 11.2, z = 0.64;
                        const wd = w * Math.sqrt(1.0 - z * z);
                        e = 1.0 - Math.exp(-z * w * t) * (Math.cos(wd * t) + (z / Math.sqrt(1.0 - z * z)) * Math.sin(wd * t));
                    }
                    pillRoot.contentAlpha = Math.max(0.0, Math.min(1.0, (t - 0.35) / 0.45));
                } else {
                    e = t * t * t;
                    pillRoot.contentAlpha = Math.max(0.0, Math.min(1.0, 1.0 - t * 2.2));
                }
                pillRoot.currentL = pillRoot.driveFrom + (pillRoot.driveTo - pillRoot.driveFrom) * e;
                if (t >= 1.0) {
                    pillRoot.driving = false;
                    pillRoot.currentL = pillRoot.driveTo;
                    if (extending) {
                        pillRoot.contentAlpha = 1.0;
                        pillRoot.glintElapsed = 0.0;
                    }
                }
            } else if (pillRoot.currentL >= pillRoot.lMax * 0.95) {
                pillRoot.contentAlpha = Math.min(1.0, pillRoot.contentAlpha + 0.08);
            }
            canvas.requestPaint();
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true
        renderStrategy: Canvas.Threaded

        function g3Points(w, h, L) {
            const K = 0.50;
            const step = (L - K * L) / 3.0;
            const pts = [];
            for (let i = 0; i < 4; i++) pts.push([w - (L - i * step), h]);
            for (let i = 3; i >= 0; i--) pts.push([w, h - (L - i * step)]);
            return pts;
        }

        function g3Eval(pts, t) {
            const B = [1, 7, 21, 35, 35, 21, 7, 1];
            const mt = 1.0 - t;
            let x = 0.0, y = 0.0;
            for (let i = 0; i < 8; i++) {
                const b = B[i] * Math.pow(mt, 7 - i) * Math.pow(t, i);
                x += b * pts[i][0];
                y += b * pts[i][1];
            }
            return [x, y];
        }

        function palette() {
            const st = pillRoot.hudState;
            const hk = pillRoot.hintKind;
            if (st === "match")
                return { rim: [76,217,100], peak: [217,255,226], bloom: [48,209,88], bpeak: [125,255,155], label: "#30d158", glyph: [48,209,88] };
            if (st === "scanning")
                return { rim: [90,200,250], peak: [216,244,255], bloom: [10,132,255], bpeak: [100,210,255], label: "#7cc8ff", glyph: [10,132,255] };
            if ((st === "partial" || st === "retry") && (hk === "too_light" || hk === "too_hard" || hk === "dry"))
                return { rim: [255,179,64], peak: [255,232,191], bloom: [255,159,10], bpeak: [255,208,128], label: "#ffb340", glyph: [255,159,10] };
            if (st === "retry" && hk === "wrong_area")
                return { rim: [100,210,255], peak: [230,249,255], bloom: [10,132,255], bpeak: [100,210,255], label: "#64d2ff", glyph: [100,210,255] };
            if (st === "retry" && (hk === "off_center" || hk === "moving"))
                return { rim: [220,230,247], peak: [255,255,255], bloom: [143,186,255], bpeak: [216,233,255], label: "#e6f0ff", glyph: [255,255,255] };
            if (st === "retry" || st === "declined")
                return { rim: [255,105,97], peak: [255,212,209], bloom: [255,69,58], bpeak: [255,140,133], label: "#ff6961", glyph: [255,94,77] };
            return { rim: [220,230,247], peak: [255,255,255], bloom: [143,186,255], bpeak: [216,233,255], label: "rgba(235,235,245,0.72)", glyph: [255,255,255] };
        }

        function rgba(rgb, a) {
            return "rgba(" + rgb[0] + "," + rgb[1] + "," + rgb[2] + "," + Math.max(0, Math.min(1, a)).toFixed(3) + ")";
        }

        function edgeFadeGrad(ctx, x1, y1, x2, y2, edgeRgb, peakRgb, maxA) {
            const g = ctx.createLinearGradient(x1, y1, x2, y2);
            g.addColorStop(0.00, rgba(edgeRgb, 0.0));
            g.addColorStop(0.16, rgba(edgeRgb, maxA * 0.36));
            g.addColorStop(0.50, rgba(peakRgb, maxA));
            g.addColorStop(0.84, rgba(edgeRgb, maxA * 0.36));
            g.addColorStop(1.00, rgba(edgeRgb, 0.0));
            return g;
        }

        function traceCurve(ctx, curve) {
            ctx.beginPath();
            ctx.moveTo(curve[0][0], curve[0][1]);
            for (let i = 1; i < curve.length; i++) ctx.lineTo(curve[i][0], curve[i][1]);
        }

        function drawFingerprint(ctx, cx, cy, strokeStyle, scale, fillFrac) {
            ctx.save();
            ctx.translate(cx, cy);
            ctx.scale(scale, scale);
            ctx.strokeStyle = strokeStyle;
            ctx.lineWidth = 2.0;
            ctx.lineCap = "round";

            const arcs = [
                [5.0,  7.2, -10, 215, 1.8],
                [10.0, 12.6,  -5, 198, 1.2],
                [10.0, 12.6, 232, 108, 1.2],
                [15.0, 17.6,  12, 172, 0.6],
                [15.0, 17.6, 212, 128, 0.6],
                [20.0, 22.8,  28, 138, 0.0],
                [20.0, 22.8, 200, 148, 0.0]
            ];

            if (fillFrac > 0.05) {
                ctx.beginPath();
                ctx.moveTo(0, 2.0);
                ctx.lineTo(0, 2.0 + 9.6 * Math.min(1.0, fillFrac * 3.0));
                ctx.stroke();
            }

            const total = arcs.length;
            for (let i = 0; i < total; i++) {
                const a = arcs[i];
                let span = a[3];
                if (fillFrac < 1.0) {
                    const local = Math.max(0.0, Math.min(1.0, (fillFrac * total - i * 0.55) / 1.4));
                    if (local <= 0.01) continue;
                    span = a[3] * local;
                }
                const startRad = -a[2] * Math.PI / 180.0;
                const endRad = -(a[2] + span) * Math.PI / 180.0;
                ctx.save();
                ctx.translate(0, a[4]);
                ctx.scale(1.0, a[1] / a[0]);
                ctx.beginPath();
                ctx.arc(0, 0, a[0], startRad, endRad, true);
                ctx.stroke();
                ctx.restore();
            }
            ctx.restore();
        }

        function drawCheckmark(ctx, cx, cy, scale, prog, strokeStyle) {
            ctx.save();
            ctx.strokeStyle = strokeStyle;
            ctx.lineWidth = 3.2 * scale;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            const p1x = cx - 11.0 * scale, p1y = cy + 1.0 * scale;
            const p2x = cx - 3.0 * scale,  p2y = cy + 9.0 * scale;
            const p3x = cx + 13.0 * scale, p3y = cy - 9.0 * scale;
            ctx.beginPath();
            ctx.moveTo(p1x, p1y);
            if (prog <= 0.42) {
                const t = prog / 0.42;
                ctx.lineTo(p1x + (p2x - p1x) * t, p1y + (p2y - p1y) * t);
            } else {
                ctx.lineTo(p2x, p2y);
                const t = (prog - 0.42) / 0.58;
                ctx.lineTo(p2x + (p3x - p2x) * t, p2y + (p3y - p2y) * t);
            }
            ctx.stroke();
            ctx.restore();
        }

        function drawCurvedText(ctx, curve, w, h, r, text, color) {
            if (!text) return;
            const off = Math.max(18.0, r * 0.30);
            const inner = [];
            const n = curve.length;
            for (let i = 0; i < n; i++) {
                const a = curve[Math.max(0, i - 1)];
                const b = curve[Math.min(n - 1, i + 1)];
                const tx = b[0] - a[0], ty = b[1] - a[1];
                const norm = Math.hypot(tx, ty) || 1.0;
                let nx = -ty / norm, ny = tx / norm;
                if ((w - curve[i][0]) * nx + (h - curve[i][1]) * ny < 0.0) {
                    nx = -nx; ny = -ny;
                }
                inner.push([curve[i][0] + nx * off, curve[i][1] + ny * off]);
            }
            const cum = [0.0];
            for (let i = 1; i < inner.length; i++) {
                cum.push(cum[i - 1] + Math.hypot(inner[i][0] - inner[i - 1][0], inner[i][1] - inner[i - 1][1]));
            }
            const totalLen = cum[cum.length - 1];
            if (totalLen < 40.0) return;

            const px = Math.round(Math.max(12.0, Math.min(14.5, r * 0.21)));
            ctx.font = "600 " + px + "px sans-serif";
            ctx.fillStyle = color;
            ctx.textAlign = "center";
            ctx.textBaseline = "alphabetic";

            const widths = [];
            let textW = 0.0;
            for (let i = 0; i < text.length; i++) {
                const cw = ctx.measureText(text[i]).width;
                widths.push(cw);
                textW += cw;
            }
            let s = (totalLen - textW) * 0.5;
            for (let i = 0; i < text.length; i++) {
                const target = Math.max(0.0, Math.min(totalLen, s + widths[i] * 0.5));
                let lo = 0, hi = cum.length - 1;
                while (lo < hi - 1) {
                    const mid = (lo + hi) >> 1;
                    if (cum[mid] <= target) lo = mid; else hi = mid;
                }
                const seg = (cum[hi] - cum[lo]) || 1.0;
                const f = (target - cum[lo]) / seg;
                const x = inner[lo][0] + (inner[hi][0] - inner[lo][0]) * f;
                const y = inner[lo][1] + (inner[hi][1] - inner[lo][1]) * f;
                const ang = Math.atan2(inner[hi][1] - inner[lo][1], inner[hi][0] - inner[lo][0]);
                ctx.save();
                ctx.translate(x, y);
                ctx.rotate(ang);
                ctx.fillText(text[i], 0, 0);
                ctx.restore();
                s += widths[i];
            }
        }

        onPaint: {
            const ctx = getContext("2d");
            const w = width, h = height;
            ctx.clearRect(0, 0, w, h);

            const L = pillRoot.currentL;
            if (L < 0.8) return;

            const pts = g3Points(w, h, L);
            const curve = [];
            const steps = 100;
            for (let i = 0; i <= steps; i++) curve.push(g3Eval(pts, i / steps));

            const pal = palette();
            const st = pillRoot.hudState;
            const hk = pillRoot.hintKind;
            const dt = pillRoot.stateElapsed;
            const ph = pillRoot.phase;

            // 1. Outer-only OLED ambient bloom behind #000000 shape
            const breath = 0.5 + 0.5 * Math.sin(ph * 2.1);
            let bloomMult = 0.72;
            if (st === "armed") bloomMult = 0.42 + 0.24 * breath;
            else if (st === "match") bloomMult = 0.95;
            else if (st === "scanning" || st === "partial" || hk) bloomMult = 0.65 + 0.20 * Math.sin(ph * 3.5);
            else if (st === "declined") bloomMult = 0.34;

            const bloomPasses = [[26.0, 0.04], [18.0, 0.07], [12.0, 0.12], [7.0, 0.19], [3.8, 0.30]];
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            for (let i = 0; i < bloomPasses.length; i++) {
                traceCurve(ctx, curve);
                ctx.lineWidth = bloomPasses[i][0];
                ctx.strokeStyle = edgeFadeGrad(ctx, w - L, h, w, h - L, pal.bloom, pal.bpeak, bloomPasses[i][1] * bloomMult);
                ctx.stroke();
            }

            // 2. Pure #000000 OLED hardware cutout
            traceCurve(ctx, curve);
            ctx.lineTo(w + 4, h - L);
            ctx.lineTo(w + 4, h + 4);
            ctx.lineTo(w - L, h + 4);
            ctx.closePath();
            ctx.fillStyle = "#000000";
            ctx.fill();

            // 3. Specular Dynamic Island keyline along exact G3 curve (0% at bezel ends)
            traceCurve(ctx, curve);
            ctx.lineWidth = 1.35;
            ctx.strokeStyle = edgeFadeGrad(ctx, w - L, h, w, h - L, pal.rim, pal.peak, (st === "match" || st === "scanning") ? 0.92 : 0.80);
            ctx.stroke();

            // 4. Traveling soft specular glint on extend / match
            if (pillRoot.glintElapsed >= 0.0 && pillRoot.glintElapsed <= pillRoot.glintDuration) {
                const gt = pillRoot.glintElapsed / pillRoot.glintDuration;
                const ge = gt < 0.5 ? (2 * gt * gt) : (1 - Math.pow(-2 * gt + 2, 2) / 2);
                const center = 0.12 + ge * 0.76;
                const env = Math.sin(gt * Math.PI);
                const gg = ctx.createLinearGradient(w - L, h, w, h - L);
                gg.addColorStop(Math.max(0.0, center - 0.16), "rgba(255,255,255,0)");
                gg.addColorStop(center, "rgba(255,255,255," + (0.85 * env).toFixed(3) + ")");
                gg.addColorStop(Math.min(1.0, center + 0.16), "rgba(255,255,255,0)");
                traceCurve(ctx, curve);
                ctx.lineWidth = 2.2;
                ctx.strokeStyle = gg;
                ctx.stroke();
            }

            // 5. Clipped interior: glyph, animations & curved label
            if (pillRoot.contentAlpha <= 0.01) return;
            ctx.save();
            traceCurve(ctx, curve);
            ctx.lineTo(w + 4, h - L);
            ctx.lineTo(w + 4, h + 4);
            ctx.lineTo(w - L, h + 4);
            ctx.closePath();
            ctx.clip();
            ctx.globalAlpha = pillRoot.contentAlpha;

            const mid = g3Eval(pts, 0.5);
            const D = Math.hypot(w - mid[0], h - mid[1]);
            const r = D / (1.0 + Math.SQRT2);
            const cx = w - r * 0.86;
            const cy = h - r * 0.86;
            const scale = (r * 0.43) / 22.8;

            if (st === "match") {
                const ridgeA = Math.max(0.0, 1.0 - Math.max(0.0, dt - 0.12) / 0.22);
                if (ridgeA > 0.01) drawFingerprint(ctx, cx, cy, rgba(pal.glyph, ridgeA), scale, 1.0);
                const prog = Math.max(0.0, Math.min(1.0, (dt - 0.16) / 0.32));
                if (prog > 0.0) drawCheckmark(ctx, cx, cy, scale * 1.12, prog, rgba(pal.glyph, 1.0));
            } else if (hk === "too_light" || hk === "too_hard") {
                const inward = (hk === "too_light");
                const period = inward ? 1.2 : 1.3;
                for (let i = 0; i < 3; i++) {
                    const t = ((ph / period) + i / 3.0) % 1.0;
                    const sc = inward ? (1.42 - 0.60 * t) : (0.84 + 0.58 * t);
                    const a = inward ? (Math.sin(t * Math.PI) * 0.82) : ((1.0 - t) * 0.85);
                    ctx.beginPath();
                    ctx.arc(cx, cy, 25.0 * scale * sc, 0, Math.PI * 2);
                    ctx.lineWidth = 1.5 * scale;
                    ctx.strokeStyle = rgba(pal.glyph, a);
                    ctx.stroke();
                }
                const pulse = 0.5 - 0.5 * Math.cos(ph * (2.0 * Math.PI / period));
                const gscale = scale * (inward ? (1.0 - 0.12 * pulse) : (0.90 + 0.14 * pulse));
                drawFingerprint(ctx, cx, cy, rgba(pal.glyph, 0.95), gscale, 1.0);
            } else if (hk === "wrong_area") {
                ctx.save();
                ctx.setLineDash([3.5 * scale, 4.5 * scale]);
                ctx.beginPath();
                ctx.arc(cx, cy, 27.0 * scale, 0, Math.PI * 2);
                ctx.lineWidth = 1.4 * scale;
                ctx.strokeStyle = "rgba(100,210,255,0.28)";
                ctx.stroke();
                ctx.restore();
                const ang = ph * 2.8;
                const ox = Math.sin(ang) * 5.2 * scale;
                const oy = Math.sin(ang * 2.0) * 3.2 * scale;
                drawFingerprint(ctx, cx + ox, cy + oy, rgba(pal.glyph, 0.95), scale, 1.0);
            } else if (hk === "off_center") {
                ctx.beginPath();
                ctx.arc(cx, cy, 26.0 * scale, 0, Math.PI * 2);
                ctx.lineWidth = 1.3 * scale;
                ctx.strokeStyle = "rgba(255,255,255,0.25)";
                ctx.stroke();
                const t = (ph / 1.3) % 1.0;
                const ease = 1.0 - Math.pow(1.0 - t, 3);
                const gx = (1.0 - ease) * (-12.0 * scale);
                drawFingerprint(ctx, cx, cy, "rgba(255,255,255,0.36)", scale, 1.0);
                drawFingerprint(ctx, cx + gx, cy, rgba([255,255,255], 0.85 * Math.sin(t * Math.PI)), scale, 1.0);
            } else if (st === "scanning") {
                drawFingerprint(ctx, cx, cy, "rgba(255,255,255,0.28)", scale, 1.0);
                const fillProg = Math.min(1.0, (dt % 0.95) / 0.62);
                drawFingerprint(ctx, cx, cy, "#0a84ff", scale, fillProg);
            } else if (st === "retry") {
                const shakeX = dt < 0.48 ? (Math.sin(dt * 42.0) * 6.5 * Math.exp(-dt * 7.5)) : 0.0;
                drawFingerprint(ctx, cx + shakeX, cy, rgba(pal.glyph, 0.95), scale, 1.0);
            } else if (st === "declined") {
                drawFingerprint(ctx, cx, cy, rgba(pal.glyph, 0.48), scale, 1.0);
            } else {
                const b = 0.74 + 0.26 * (0.5 + 0.5 * Math.sin(ph * 2.1));
                drawFingerprint(ctx, cx, cy, rgba([255,255,255], 0.94 * b), scale, 1.0);
            }

            drawCurvedText(ctx, curve, w, h, r, pillRoot.curveLabel, pal.label);
            ctx.restore();
        }
    }
}
