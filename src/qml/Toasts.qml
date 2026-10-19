import QtQuick
import QtQuick.Layouts

// Notices at the top right: a new polyphony or NPS peak once it has passed, and
// a lag warning naming the parts that run out of frames or fall behind. Checked
// once per second while playing.
Column {
    id: toasts
    required property var mainWindow
    required property var roll
    required property var dock
    readonly property var mw: mainWindow
    spacing: 8
    width: 300

    property int windowFrames: 0
    property int timeUpdates: 0
    property real displayRate: 0
    property int lastUnderruns: 0
    property int badSeconds: 0
    property int goodSeconds: 0
    property real lagMutedUntil: 0
    property var lagRows: []
    property string lagNames: ""

    property int polyShown: 0
    property int polyPending: 0
    property real polyPendingSince: 0
    property bool polyMuted: false
    property int npsShown: 0
    property int npsPending: 0
    property real npsPendingSince: 0
    property bool npsMuted: false

    property var revision: mw.documentRevision
    onRevisionChanged: {
        polyToast.visible = npsToast.visible = lagToast.visible = false
        polyShown = polyPending = npsShown = npsPending = 0
        polyMuted = npsMuted = false
        displayRate = 0
        badSeconds = goodSeconds = 0
    }

    FrameAnimation {
        running: toasts.mw.isPlaying
        onTriggered: ++toasts.windowFrames
    }

    // The graph can only draw as often as the playback clock moves.
    Connections {
        target: toasts.mw
        function onCurrentTimeChanged() { ++toasts.timeUpdates }
    }

    function clock(seconds) {
        var m = Math.floor(seconds / 60)
        var s = seconds - m * 60
        return m + ":" + (s < 10 ? "0" : "") + s.toFixed(1)
    }

    // A peak counts once nothing has beaten it for 1.5 s.
    function checkPeaks(now) {
        if (mw.peakPolyphony > polyPending) {
            polyPending = mw.peakPolyphony
            polyPendingSince = now
        }
        if (!polyMuted && polyPending > polyShown && now - polyPendingSince >= 1.5 && mw.currentTime >= 3) {
            polyShown = polyPending
            polyToast.text = "<span style='font-family: VT323; font-size: 24px; color: " + Dk.white + ";'>" +
                Dk.integer(polyShown) + "</span> notes at once, the most so far, at " + clock(mw.peakPolyphonyTime) + "."
            polyToast.show()
        }
        if (mw.peakNps > npsPending) {
            npsPending = mw.peakNps
            npsPendingSince = now
        }
        if (!npsMuted && npsPending > npsShown && now - npsPendingSince >= 1.5 && mw.currentTime >= 3) {
            npsShown = npsPending
            npsToast.text = "<span style='font-family: VT323; font-size: 24px; color: " + Dk.white + ";'>" +
                Dk.integer(npsShown) + "</span> notes per second, the most so far, at " + clock(mw.peakNpsTime) + "."
            npsToast.show()
        }
    }

    function checkLag(now) {
        var frames = windowFrames
        var clockRate = timeUpdates
        windowFrames = timeUpdates = 0
        // The display rate is the best the page has managed.
        displayRate = Math.max(displayRate, frames)
        var target = displayRate * 0.8
        var graphFrames = dock.takeGraphFrames()
        var keys = mw.takeKeyboardSample()
        var underruns = mw.synthUnderruns - lastUnderruns
        lastUnderruns = mw.synthUnderruns

        var rows = []
        var names = []
        function row(name, bad, text) {
            rows.push({ name: name, bad: bad, text: bad ? text : "ON TIME" })
            if (bad)
                names.push(name.charAt(0) + name.slice(1).toLowerCase())
        }
        row("ROLL", roll.fps < target, roll.fps + " FPS")
        row("KEYBOARD", keys.lag > 0.25 || keys.frames < Math.min(target, 30),
            keys.frames + " FPS" + (keys.lag > 0.25 ? " · " + keys.lag.toFixed(1) + " S BEHIND" : ""))
        row("GRAPHS", graphFrames < 0.8 * Math.min(frames, clockRate), graphFrames + " FPS")
        row("WASMISYNTH", underruns > 0 || mw.synthRenderLoad > 100,
            underruns > 0 ? underruns + " UNDERRUNS" : Math.round(mw.synthRenderLoad) + "% LOAD")

        if (names.length) {
            ++badSeconds
            goodSeconds = 0
        } else {
            ++goodSeconds
            badSeconds = 0
        }
        if (badSeconds >= 2 && now >= lagMutedUntil) {
            lagRows = rows
            lagNames = names.join(", ")
            lagToast.visible = true
        } else if (lagToast.visible && badSeconds === 0) {
            lagRows = rows
            lagNames = ""
            if (goodSeconds >= 4)
                lagToast.visible = false
        }
    }

    Timer {
        interval: 1000
        repeat: true
        running: toasts.mw.isPlaying
        onRunningChanged: {
            toasts.windowFrames = toasts.timeUpdates = 0
            toasts.badSeconds = toasts.goodSeconds = 0
            toasts.lastUnderruns = toasts.mw.synthUnderruns
            toasts.dock.takeGraphFrames()
            toasts.mw.takeKeyboardSample()
        }
        onTriggered: {
            var now = Date.now() / 1000
            toasts.checkPeaks(now)
            toasts.checkLag(now)
        }
    }

    component Toast: Rectangle {
        id: toast
        property string title
        property var iconRects: []
        property color iconColor: Dk.lav
        property color startColor: Dk.captionA
        property color endColor: Dk.accent
        property string text
        property int autoHide: 8000
        default property alias body: extra.data
        signal dismissed()

        function show() {
            visible = true
            hideTimer.restart()
        }

        visible: false
        width: toasts.width
        height: column.implicitHeight + 6
        color: Dk.face
        DkBevel { anchors.fill: parent; kind: 0 }

        Timer {
            id: hideTimer
            interval: toast.autoHide
            running: false
            onTriggered: if (toast.autoHide > 0) toast.visible = false
        }

        ColumnLayout {
            id: column
            x: 3
            y: 3
            width: parent.width - 6
            spacing: 0
            DkCaption {
                Layout.fillWidth: true
                title: toast.title
                startColor: toast.startColor
                endColor: toast.endColor
                iconRects: toast.iconRects.map(function (r) { return r.concat([toast.iconColor]) })
                DkCapButton {
                    glyph: Glyphs.close
                    onClicked: {
                        toast.visible = false
                        toast.dismissed()
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                Layout.margins: 8
                visible: toast.text.length > 0
                text: toast.text
                textFormat: Text.RichText
                wrapMode: Text.Wrap
                color: Dk.text
                font.family: Dk.ui
                font.pixelSize: 12
            }
            ColumnLayout {
                id: extra
                Layout.fillWidth: true
                Layout.margins: 8
                Layout.topMargin: toast.text.length > 0 ? 0 : 6
                visible: children.length > 0
            }
        }
    }

    Toast {
        id: polyToast
        title: "Max polyphony reached"
        iconRects: [[1, 7, 2, 4], [5, 3, 2, 8], [9, 1, 2, 10]]
        onDismissed: toasts.polyMuted = true
    }

    Toast {
        id: npsToast
        title: "Max NPS reached"
        iconColor: Dk.warn
        iconRects: [[7, 0, 2, 2], [5, 2, 2, 3], [3, 5, 6, 2], [5, 7, 2, 3], [3, 10, 2, 2]]
        onDismissed: toasts.npsMuted = true
    }

    Toast {
        id: lagToast
        title: "Lag warning"
        startColor: "#4a1a3a"
        endColor: "#b5426e"
        iconColor: "#ffd1e0"
        iconRects: [[5, 0, 2, 2], [4, 2, 4, 2], [3, 4, 6, 3], [2, 7, 8, 2], [1, 9, 10, 3]]
        autoHide: 0
        onDismissed: toasts.lagMutedUntil = Date.now() / 1000 + 30

        Text {
            Layout.fillWidth: true
            Layout.bottomMargin: 5
            text: toasts.lagNames.length
                ? "Out of frames: <b><font color='" + Dk.pink + "'>" + toasts.lagNames + "</font></b>"
                : "Caught up."
            textFormat: Text.RichText
            color: Dk.text
            font.family: Dk.ui
            font.pixelSize: 12
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: grid.implicitHeight + 6
            color: Dk.readout
            DkBevel { anchors.fill: parent; kind: 2 }
            GridLayout {
                id: grid
                x: 6
                y: 3
                width: parent.width - 12
                columns: 2
                rowSpacing: 1
                columnSpacing: 10
                Repeater {
                    model: toasts.lagRows.length * 2
                    Text {
                        required property int index
                        readonly property var row: toasts.lagRows[Math.floor(index / 2)]
                        Layout.fillWidth: index % 2 === 0
                        text: index % 2 === 0 ? row.name : row.text
                        color: index % 2 === 0 ? Dk.dim : (row.bad ? Dk.bad : Dk.good)
                        font.family: Dk.term
                        font.pixelSize: 16
                    }
                }
            }
        }
    }
}
