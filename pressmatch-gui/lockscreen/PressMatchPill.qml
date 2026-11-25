/*
 * PressMatchPill.qml — Touch ID-Style Floating Fingerprint HUD for KDE Plasma 6 Lockscreen
 *
 * Visual twin of pressmatch_hud.py:
 *   - armed    : Frosted porcelain pill + breathing blue fingerprint orb + active readers
 *   - scanning : Electric blue orb + sweeping scan beam + expanding radar ripple
 *   - match    : Emerald green orb + animated checkmark + celebratory burst
 *   - retry    : Coral-red orb + damped horizontal shake + auto-revert to ready
 */
import QtQuick
import QtQuick.Layouts

Item {
    id: pillRoot

    // Public state API: "hidden" | "armed" | "scanning" | "match" | "retry"
    property string hudState: "hidden"
    property string activeDevice: "ELAN · FT9201"
    property string readersLabel: "ELAN · FT9201"
    property int retryCount: 0
    property int maxRetries: 5

    readonly property bool isDe: Qt.locale().name.toLowerCase().startsWith("de")
    readonly property int pillW: 324
    readonly property int pillH: 68

    width: pillW + 36
    height: pillH + 36
    visible: opacity > 0.01
    opacity: hudState === "hidden" ? 0.0 : 1.0

    property real phase: 0.0
    property real stateElapsed: 0.0
    property real shakeX: 0.0
    property real ySlide: hudState === "hidden" ? -10.0 : 0.0

    Behavior on opacity {
        NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
    }
    Behavior on ySlide {
        NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
    }

    function setArmed(readersText) {
        if (readersText && readersText.length > 0) {
            readersLabel = readersText;
        }
        activeDevice = readersLabel;
        hudState = "armed";
        stateElapsed = 0.0;
        retryRevertTimer.stop();
        canvas.requestPaint();
    }

    function setScanning(devText) {
        if (devText && devText.length > 0) {
            activeDevice = devText;
        }
        hudState = "scanning";
        stateElapsed = 0.0;
        retryRevertTimer.stop();
        canvas.requestPaint();
    }

    function setMatch(devText) {
        if (devText && devText.length > 0) {
            activeDevice = devText;
        }
        hudState = "match";
        stateElapsed = 0.0;
        retryRevertTimer.stop();
        canvas.requestPaint();
    }

    function setRetry(devText) {
        retryCount = retryCount + 1;
        if (devText && devText.length > 0) {
            activeDevice = devText;
        }
        hudState = "retry";
        stateElapsed = 0.0;
        retryRevertTimer.restart();
        canvas.requestPaint();
    }

    function parsePamMessage(msg) {
        if (!msg) return false;
        const s = String(msg).trim();
        if (!s) return false;
        const low = s.toLowerCase();

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
        if (low.indexOf("failed to match") !== -1 || low.indexOf("try again") !== -1) {
            setRetry(activeDevice);
            return true;
        }
        return false;
    }

    Timer {
        id: retryRevertTimer
        interval: 1200
        repeat: false
        onTriggered: {
            if (pillRoot.hudState === "retry") {
                pillRoot.hudState = "armed";
                pillRoot.activeDevice = pillRoot.readersLabel;
                pillRoot.stateElapsed = 0.0;
                canvas.requestPaint();
            }
        }
    }

    // 60 FPS animation driver — active only while pill is visible
    Timer {
        id: animTimer
        interval: 16
        repeat: true
        running: pillRoot.visible && pillRoot.hudState !== "hidden"
        onTriggered: {
            pillRoot.phase += 0.016;
            pillRoot.stateElapsed += 0.016;
            if (pillRoot.hudState === "retry" && pillRoot.stateElapsed < 0.42) {
                const decay = Math.exp(-pillRoot.stateElapsed * 8.5);
                pillRoot.shakeX = Math.sin(pillRoot.stateElapsed * 42.0) * 9.5 * decay;
            } else {
                pillRoot.shakeX = 0.0;
            }
            canvas.requestPaint();
        }
    }

    readonly property string titleText: {
        if (hudState === "scanning") return isDe ? "Scanne Fingerabdruck…" : "Scanning Fingerprint…";
        if (hudState === "match") return isDe ? "Entsperrt" : "Unlocked";
        if (hudState === "retry") {
            const base = isDe ? "Nochmal versuchen" : "Try Again";
            return retryCount > 0 ? (base + " (" + retryCount + "/" + maxRetries + ")") : base;
        }
        return isDe ? "Fingerabdruck bereit" : "Fingerprint Ready";
    }

    readonly property color titleColor: {
        if (hudState === "scanning") return "#0066E0";
        if (hudState === "match") return "#168A2D";
        if (hudState === "retry") return "#CC3E26";
        return "#1C1C1E";
    }

    readonly property color borderColor: {
        if (hudState === "scanning") return Qt.rgba(0, 122/255, 1, 0.48);
        if (hudState === "match") return Qt.rgba(40/255, 205/255, 65/255, 0.55);
        if (hudState === "retry") return Qt.rgba(1, 107/255, 74/255, 0.58);
        return Qt.rgba(15/255, 23/255, 42/255, 0.14);
    }

    // Main frosted porcelain pill container
    Item {
        id: pillFrame
        x: 18 + pillRoot.shakeX
        y: 18 + pillRoot.ySlide
        width: pillRoot.pillW
        height: pillRoot.pillH

        // Soft layered ambient shadow
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            anchors.topMargin: 2
            anchors.bottomMargin: -6
            radius: parent.height / 2 + 3
            color: Qt.rgba(0, 0, 0, 0.16)
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -1
            anchors.topMargin: 1
            anchors.bottomMargin: -3
            radius: parent.height / 2 + 1
            color: Qt.rgba(0, 0, 0, 0.18)
        }

        // Porcelain pill surface
        Rectangle {
            anchors.fill: parent
            radius: height / 2
            border.width: 1.25
            border.color: pillRoot.borderColor
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(1.0, 1.0, 1.0, 0.96) }
                GradientStop { position: 1.0; color: Qt.rgba(246/255, 247/255, 250/255, 0.94) }
            }

            // Left animated vector orb + right pulse indicator rendered on crisp HiDPI Canvas
            Canvas {
                id: canvas
                anchors.fill: parent
                antialiasing: true
                renderStrategy: Canvas.Threaded

                function drawFingerprint(ctx, cx, cy, strokeStyle) {
                    ctx.save();
                    ctx.translate(cx, cy);
                    ctx.strokeStyle = strokeStyle;
                    ctx.lineWidth = 1.75;
                    ctx.lineCap = "round";

                    const arcs = [
                        [11.5, 13.0, 28, 138, 1.0],
                        [11.5, 13.0, 200, 42, 1.0],
                        [8.5, 10.0, 12, 175, 1.2],
                        [8.5, 10.0, 212, 95, 1.2],
                        [5.5, 7.0, -5, 205, 1.4],
                        [5.5, 7.0, 232, 85, 1.4],
                        [2.6, 3.8, -10, 215, 1.6]
                    ];
                    for (let i = 0; i < arcs.length; i++) {
                        const a = arcs[i];
                        const rx = a[0], ry = a[1];
                        const startRad = -a[2] * Math.PI / 180.0;
                        const endRad = -(a[2] + a[3]) * Math.PI / 180.0;
                        ctx.save();
                        ctx.translate(0, a[4]);
                        ctx.scale(1.0, ry / rx);
                        ctx.beginPath();
                        ctx.arc(0, 0, rx, startRad, endRad, true);
                        ctx.stroke();
                        ctx.restore();
                    }
                    ctx.beginPath();
                    ctx.moveTo(0, 1.5);
                    ctx.lineTo(0, 6.8);
                    ctx.stroke();
                    ctx.restore();
                }

                onPaint: {
                    const ctx = getContext("2d");
                    ctx.clearRect(0, 0, width, height);

                    const cx = 36.0;
                    const cy = height / 2.0;
                    const r = 21.0;
                    const st = pillRoot.hudState;
                    const dt = pillRoot.stateElapsed;
                    const ph = pillRoot.phase;

                    if (st === "armed") {
                        const breath = 0.5 + 0.5 * Math.sin(ph * 2.8);
                        ctx.beginPath();
                        ctx.arc(cx, cy, r + 2.0 + 3.5 * breath, 0, Math.PI * 2);
                        ctx.strokeStyle = "rgba(0, 122, 255, " + (0.18 + 0.20 * breath).toFixed(3) + ")";
                        ctx.lineWidth = 1.6;
                        ctx.stroke();

                        ctx.beginPath();
                        ctx.arc(cx, cy, r, 0, Math.PI * 2);
                        ctx.fillStyle = "rgba(0, 122, 255, 0.09)";
                        ctx.fill();

                        drawFingerprint(ctx, cx, cy, "#0070F3");

                        // Right subtle pulsing dot
                        const dotX = width - 22.0;
                        const pulse = 0.45 + 0.55 * breath;
                        ctx.beginPath();
                        ctx.arc(dotX, cy, 6.5, 0, Math.PI * 2);
                        ctx.fillStyle = "rgba(0, 122, 255, " + (0.25 * pulse).toFixed(3) + ")";
                        ctx.fill();
                        ctx.beginPath();
                        ctx.arc(dotX, cy, 3.3, 0, Math.PI * 2);
                        ctx.fillStyle = "rgba(0, 122, 255, " + (0.75 * pulse).toFixed(3) + ")";
                        ctx.fill();
                    } else if (st === "scanning") {
                        const rip = (dt * 1.8) % 1.0;
                        ctx.beginPath();
                        ctx.arc(cx, cy, r + rip * 9.0, 0, Math.PI * 2);
                        ctx.strokeStyle = "rgba(0, 122, 255, " + (0.68 * (1.0 - rip)).toFixed(3) + ")";
                        ctx.lineWidth = 2.0;
                        ctx.stroke();

                        const g = ctx.createRadialGradient(cx, cy - 6, 2, cx, cy, r * 1.4);
                        g.addColorStop(0.0, "#2496FF");
                        g.addColorStop(1.0, "#0066EB");
                        ctx.beginPath();
                        ctx.arc(cx, cy, r, 0, Math.PI * 2);
                        ctx.fillStyle = g;
                        ctx.fill();

                        drawFingerprint(ctx, cx, cy, "rgba(255, 255, 255, 0.92)");

                        // Sweeping scan beam clipped inside orb
                        ctx.save();
                        ctx.beginPath();
                        ctx.arc(cx, cy, r - 1.0, 0, Math.PI * 2);
                        ctx.clip();
                        const sweep = Math.sin(ph * 6.5) * (r * 0.72);
                        const bg = ctx.createLinearGradient(cx, cy + sweep - 6, cx, cy + sweep + 6);
                        bg.addColorStop(0.0, "rgba(255,255,255,0)");
                        bg.addColorStop(0.5, "rgba(255,255,255,0.65)");
                        bg.addColorStop(1.0, "rgba(255,255,255,0)");
                        ctx.fillStyle = bg;
                        ctx.fillRect(cx - r, cy + sweep - 6, r * 2, 12);
                        ctx.restore();

                        const dotX = width - 22.0;
                        const pulse = 0.5 + 0.5 * Math.sin(ph * 10.0);
                        ctx.beginPath();
                        ctx.arc(dotX, cy, 4.8 + 2.0 * pulse, 0, Math.PI * 2);
                        ctx.fillStyle = "rgba(0, 122, 255, " + (0.35 + 0.45 * pulse).toFixed(3) + ")";
                        ctx.fill();
                    } else if (st === "match") {
                        const burst = Math.min(1.0, dt / 0.45);
                        if (burst < 1.0) {
                            ctx.beginPath();
                            ctx.arc(cx, cy, r + burst * 11.0, 0, Math.PI * 2);
                            ctx.strokeStyle = "rgba(40, 205, 65, " + (0.75 * (1.0 - burst)).toFixed(3) + ")";
                            ctx.lineWidth = 2.2;
                            ctx.stroke();
                        }
                        const g = ctx.createRadialGradient(cx, cy - 6, 2, cx, cy, r * 1.4);
                        g.addColorStop(0.0, "#34DA59");
                        g.addColorStop(1.0, "#1EB23C");
                        ctx.beginPath();
                        ctx.arc(cx, cy, r, 0, Math.PI * 2);
                        ctx.fillStyle = g;
                        ctx.fill();

                        const prog = Math.min(1.0, dt / 0.24);
                        ctx.strokeStyle = "#FFFFFF";
                        ctx.lineWidth = 2.8;
                        ctx.lineCap = "round";
                        ctx.lineJoin = "round";
                        ctx.beginPath();
                        const p1x = cx - 7.5, p1y = cy + 0.5;
                        const p2x = cx - 2.0, p2y = cy + 6.0;
                        const p3x = cx + 8.5, p3y = cy - 5.5;
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
                    } else if (st === "retry") {
                        const g = ctx.createRadialGradient(cx, cy - 6, 2, cx, cy, r * 1.4);
                        g.addColorStop(0.0, "#FF7A59");
                        g.addColorStop(1.0, "#E54834");
                        ctx.beginPath();
                        ctx.arc(cx, cy, r, 0, Math.PI * 2);
                        ctx.fillStyle = g;
                        ctx.fill();

                        drawFingerprint(ctx, cx, cy, "rgba(255, 255, 255, 0.92)");
                    }
                }
            }

            // Text stack
            Column {
                anchors.left: parent.left
                anchors.leftMargin: 72
                anchors.right: parent.right
                anchors.rightMargin: 34
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    width: parent.width
                    text: pillRoot.titleText
                    color: pillRoot.titleColor
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: pillRoot.activeDevice
                    color: "#6E737D"
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
            }
        }
    }
}
