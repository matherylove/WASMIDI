import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// WasmiSynth engine settings, live counters and file details.
DkDialog {
    id: dialog
    required property var mainWindow
    readonly property var mw: mainWindow
    property var toasts: null
    title: "WasmiSynth — Advanced"
    width: Math.min(parent ? parent.width - 24 : 640, 640)

    component Label: Text {
        color: Dk.dim
        font.family: Dk.ui
        font.pixelSize: 12
    }

    component NumberField: DkField {
        id: field
        property string key: ""
        Layout.preferredWidth: 72
        enabled: !dialog.mw.isPlaying
        text: String(dialog.mw[key])
        onEditingFinished: {
            var n = parseInt(text)
            if (isFinite(n))
                dialog.mw[key] = n
            text = String(dialog.mw[key])
        }
    }

    component Toggle: DkButton {
        property string key: ""
        lcd: true
        on: !!dialog.mw[key]
        onClicked: dialog.mw[key] = !dialog.mw[key]
    }

    readonly property var missReasons: ["", "INTERP", "LOOP", "FILTER", "OTHER"]

    // [label, value, color, shown only with Debug]
    function counter(i) {
        var m = mw
        var dim = Dk.dim, warn = Dk.warn, bad = Dk.bad
        switch (i) {
        case 0: return ["RATE", m.synthSampleRate > 0 ? Dk.integer(m.synthSampleRate) + " Hz" : "—", dim, false]
        case 1: return ["ACTIVE", Dk.integer(m.synthActiveVoices), dim, false]
        case 2: return ["FREE", Dk.integer(m.synthFreeVoices), dim, false]
        case 3: return ["WORKERS", m.synthWorkerCount > 0 ? Dk.integer(m.synthWorkerCount) : "—", dim, false]
        case 4: return ["VOICES/WKR", Dk.integer(Math.floor(m.synthMaxVoices / Math.max(1, m.synthWorkerCount))), dim, false]
        case 5: return ["REGIONS", Dk.integer(m.synthRegions), dim, false]
        case 6: return ["LOAD", Dk.number(m.synthRenderLoad, 0) + "%", m.synthRenderLoad > 90 ? warn : dim, false]
        case 7: return ["BLOCK", Dk.number(m.synthRenderLatencyMs, 1) + " ms", dim, false]
        case 8: return ["STEALS/s", Dk.integer(m.synthSteals), m.synthSteals > 0 ? warn : dim, false]
        case 9: return ["DROPPED", Dk.integer(m.synthDroppedNotes), m.synthDroppedNotes > 0 ? bad : dim, false]
        case 10: return ["UNDERRUNS", Dk.integer(m.synthUnderruns), m.synthUnderruns > 0 ? warn : dim, false]
        case 11: return ["RING", Dk.integer(m.synthRingFill) + "%", m.synthRingFill < 20 ? warn : dim, false]
        case 12: return ["LATE", Dk.integer(m.synthPumpGapMs) + " ms", m.synthPumpGapMs > 20 ? bad : (m.synthPumpGapMs > 0 ? warn : dim), false]
        case 13: return ["EVT", Dk.number(m.synthDispatchMs, 1) + " ms", m.synthDispatchMs > m.synthRenderLatencyMs * 0.4 ? bad : dim, true]
        case 14: return ["BUSY", Dk.number(m.synthWorkerBusyMs, 0) + " ms", dim, true]
        case 15: return ["ALLOC", Dk.number(m.synthAllocMs, 0) + " ms", m.synthAllocMs > m.synthWorkerBusyMs ? bad : dim, true]
        case 16: return ["WACT", Dk.integer(m.synthWorkersActive),
                         m.synthWorkersActive > 0 && m.synthWorkersActive < m.synthWorkerCount / 2 ? bad : dim, true]
        case 17: return ["SIMD", Dk.integer(m.synthSimdPercent) + "%", m.synthSimdPercent < 50 ? bad : dim, true]
        case 18: return ["RECYC", Dk.integer(m.synthFreeRatio) + "%", m.synthFreeRatio < 80 ? bad : dim, true]
        case 19: return ["REBAL/s", Dk.integer(m.synthRebalanced), dim, true]
        case 20: return ["CCOL", Dk.integer(m.synthCcCollapsed), dim, true]
        case 21: return ["PRESMP", Dk.integer(m.synthPresampleSeen) + " / " + Dk.integer(m.synthPresampleResampled)
                         + " / " + Dk.integer(m.synthPresampleSkipped), m.synthPresampleSkipped > 0 ? bad : dim, true]
        case 22: return ["CC#" + Dk.integer(m.synthCcHotNumber), m.synthCcHotPercent > 0
                         ? Dk.integer(m.synthCcHotPercent) + "% · " + Dk.integer(m.synthCcHotCount) : "—", warn, true]
        case 24: return ["VEL ≥", Dk.integer(m.skippedVelocity),
                         m.skippedVelocity > m.synthVelocityFloor ? bad : dim, false]
        case 23: return ["MISS", m.synthMissReason > 0
                         ? missReasons[m.synthMissReason] + " " + Dk.integer(m.synthMissPercent) + "%" : "—", warn, true]
        }
        return ["", "", dim, true]
    }

    ColumnLayout {
        width: dialog.availableWidth - 8
        spacing: 15

        Item { Layout.preferredHeight: 2 }

        DkGroup {
            Layout.fillWidth: true
            title: "SoundFont"
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 21
                    color: Dk.field
                    DkBevel { anchors.fill: parent; kind: 1 }
                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        verticalAlignment: Text.AlignVCenter
                        text: dialog.mw.soundfontLoaded
                            ? dialog.mw.soundfontName + "  ·  " + Dk.integer(dialog.mw.synthLayers)
                              + (dialog.mw.synthLayers === 1 ? " layer" : " layers")
                            : "No SoundFont loaded"
                        color: dialog.mw.soundfontLoaded ? Dk.white : Dk.faint
                        font.family: Dk.ui
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }
                }
                DkButton {
                    text: "Clear"
                    enabled: dialog.mw.soundfontLoaded && !dialog.mw.isPlaying
                    onClicked: dialog.mw.clearSoundfonts()
                }
            }
            Label {
                Layout.fillWidth: true
                text: dialog.mw.synthStatus
                color: Dk.faint
                elide: Text.ElideRight
            }
        }

        DkGroup {
            Layout.fillWidth: true
            title: "Engine"
            GridLayout {
                Layout.fillWidth: true
                columns: 6
                columnSpacing: 7
                rowSpacing: 5

                Label { text: "Block" }
                NumberField { key: "synthBufferFrames"; minValue: 1; maxValue: 65536 }
                Label { text: "Buffers" }
                NumberField { key: "synthNumBuffers"; minValue: 1; maxValue: 128 }
                Label { text: "Min voices" }
                NumberField { key: "synthMinVoices"; minValue: 0; maxValue: 5000000 }

                Label { text: "Workers" }
                NumberField {
                    key: "synthWorkers"
                    minValue: 0
                    maxValue: 256
                    ToolTip.visible: hovered
                    ToolTip.text: "0 = the threads the browser reports, minus the ones kept for the page, the MIDI parser and audio. Brave may report fewer threads than the CPU has: set the count to override it."
                }
                Label { text: "Rate" }
                NumberField {
                    key: "synthRequestedSampleRate"
                    minValue: 0
                    maxValue: 384000
                    enabled: !dialog.mw.synthReady && !dialog.mw.isPlaying
                    ToolTip.visible: hovered
                    ToolTip.text: "0 = the device rate. Set it before audio starts."
                }
                Label { text: "Vel ≥" }
                NumberField { key: "synthVelocityFloor"; minValue: 0; maxValue: 127; enabled: true }

                Label { text: "Sharding" }
                DkButton {
                    lcd: true
                    enabled: !dialog.mw.isPlaying
                    text: ["AUTO", "CHANNEL", "HASH", "SINGLE"][dialog.mw.synthNoteSharding] || "AUTO"
                    onClicked: dialog.mw.synthNoteSharding = (dialog.mw.synthNoteSharding + 1) % 4
                }
                Label { text: "Channels" }
                DkButton {
                    lcd: true
                    enabled: !dialog.mw.isPlaying
                    text: dialog.mw.synthChannels === 1 ? "MONO" : "STEREO"
                    onClicked: dialog.mw.synthChannels = dialog.mw.synthChannels === 1 ? 2 : 1
                }
                Label { text: "Bits" }
                DkButton {
                    lcd: true
                    enabled: !dialog.mw.isPlaying
                    text: String(dialog.mw.synthBitsPerSample)
                    onClicked: dialog.mw.synthBitsPerSample = dialog.mw.synthBitsPerSample === 16 ? 32 : 16
                }
            }
            Flow {
                Layout.fillWidth: true
                spacing: 3
                Toggle { key: "synthRealtimePriority"; text: "RT PRIORITY"; enabled: !dialog.mw.isPlaying }
                Toggle { key: "synthStealScoreCache"; text: "STEAL CACHE"; enabled: !dialog.mw.isPlaying }
                Toggle { key: "synthFastNoteOff"; text: "FAST NOTE-OFF"; enabled: !dialog.mw.isPlaying }
                Toggle { key: "synthValidateState"; text: "VALIDATE"; enabled: !dialog.mw.isPlaying }
                Toggle { key: "synthDebugMetrics"; text: "DEBUG" }
            }
        }

        DkGroup {
            Layout.fillWidth: true
            title: "Live"
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: counters.implicitHeight + 8
                color: Dk.lcd
                DkBevel { anchors.fill: parent; kind: 1 }
                GridLayout {
                    id: counters
                    x: 6
                    y: 4
                    width: parent.width - 12
                    columns: 4
                    columnSpacing: 10
                    rowSpacing: 0
                    Repeater {
                        model: 25
                        RowLayout {
                            required property int index
                            readonly property var c: dialog.counter(index)
                            visible: !c[3] || dialog.mw.synthDebugMetrics
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            spacing: 6
                            Text { text: parent.c[0]; color: Dk.faint; font.family: Dk.term; font.pixelSize: 16 }
                            Text {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignRight
                                text: parent.c[1]
                                color: parent.c[2] === Dk.dim ? Dk.white : parent.c[2]
                                font.family: Dk.term
                                font.pixelSize: 16
                                elide: Text.ElideLeft
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            DkGroup {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                title: "View"
                RowLayout {
                    spacing: 7
                    Label { text: "Past notes" }
                    DkButton {
                        lcd: true
                        text: dialog.mw.postBufferAuto ? "AUTO" : dialog.mw.postBuffer.toFixed(1) + " S"
                        onClicked: dialog.cyclePostBuffer()
                    }
                }
                RowLayout {
                    spacing: 7
                    Label { text: "Lag log" }
                    Toggle { key: "lagLog"; text: on ? "ON" : "OFF" }
                    DkButton {
                        text: "Download"
                        enabled: !!dialog.toasts && dialog.mw.hasMidi
                        onClicked: dialog.toasts.downloadLog()
                    }
                }
                Label {
                    visible: dialog.mw.lagLog && !!dialog.toasts
                    text: dialog.toasts ? dialog.toasts.log.lagSeconds + " s of lag in " +
                                          dialog.toasts.log.episodes.length + " episodes" : ""
                }
            }

            DkGroup {
                Layout.fillWidth: true
                Layout.preferredWidth: 2.4
                title: "File"
                visible: dialog.mw.hasMidi
                GridLayout {
                    Layout.fillWidth: true
                    columns: 4
                    columnSpacing: 10
                    rowSpacing: 0
                    Repeater {
                        model: [
                            ["Format", Dk.integer(dialog.mw.midiFormat)],
                            ["PPQ", Dk.integer(dialog.mw.ppq)],
                            ["Channels", Dk.integer(dialog.mw.activeChannelCount)],
                            ["Tempo changes", Dk.integer(dialog.mw.tempoChangeCount)],
                            ["CC events", Dk.integer(dialog.mw.controlEventCount)],
                            ["Pitch", dialog.mw.pitchRange],
                            ["Peak NPS", Dk.integer(dialog.mw.peakNps)],
                            ["Peak poly", Dk.integer(dialog.mw.peakPolyphony)]
                        ]
                        RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            spacing: 6
                            Label { text: modelData[0]; color: Dk.faint }
                            Text {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignRight
                                text: modelData[1]
                                color: Dk.lav
                                font.family: Dk.ui
                                font.pixelSize: 12
                                elide: Text.ElideLeft
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            DkButton { text: "Close"; implicitWidth: 75; onClicked: dialog.close() }
        }
    }

    readonly property var postValues: [-1, 0.0, 0.1, 0.5, 1.0, 2.0]

    function cyclePostBuffer() {
        var idx = 1
        if (!mw.postBufferAuto) {
            var current = Number(mw.postBuffer.toFixed(1))
            for (var i = 1; i < postValues.length; ++i) {
                if (Math.abs(postValues[i] - current) < 0.01) {
                    idx = i
                    break
                }
            }
            idx = (idx + 1) % postValues.length
        }
        if (postValues[idx] < 0)
            mw.setPostBufferAuto()
        else
            mw.postBuffer = postValues[idx]
    }
}
