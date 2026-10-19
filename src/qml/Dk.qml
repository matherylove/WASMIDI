pragma Singleton
import QtQuick

// The dekx.cc window kit (dekx.css) as QML values.
QtObject {
    readonly property color desk: "#07071a"
    readonly property color face: "#16132f"
    readonly property color faceHi: "#1d1940"
    readonly property color well: "#0a0920"
    readonly property color field: "#06051a"
    readonly property color lcd: "#04031a"
    readonly property color readout: "#08062a"
    readonly property color light: "#5a4cb5"
    readonly property color light2: "#2e2768"
    readonly property color dark: "#000000"
    readonly property color dark2: "#0b0922"
    readonly property color text: "#dcd8f0"
    readonly property color dim: "#a39dc9"
    readonly property color faint: "#6f6a98"
    readonly property color white: "#f3f0ff"
    readonly property color lav: "#c4b5fd"
    readonly property color violet: "#a78bfa"
    readonly property color pink: "#f0abfc"
    readonly property color good: "#7cf29a"
    readonly property color warn: "#ffc371"
    readonly property color bad: "#ff5f6d"
    readonly property color toggleOn: "#2a194b"
    readonly property color captionA: "#1f175a"

    // Accent of the caption, graphs, readouts and playhead: the color of the
    // notes playing most when "adapt" is on (MainWindow binds it).
    property color accent: "#6b55d4"
    readonly property color accentLight: mix(accent, 0.3)

    readonly property string ui: "Tahoma, Verdana, DejaVu Sans, sans-serif"
    readonly property string term: "VT323"

    readonly property string ditherFace: "qrc:/qt/qml/Wasmidi/assets/images/dither-face.png"
    // White at 13% on every other pixel: the texture of notes and keys.
    readonly property string ditherLight: "qrc:/qt/qml/Wasmidi/assets/images/dither-light.png"

    // Toward white for f > 0, toward black for f < 0 (the mockup's mix()).
    function mix(c, f) {
        if (f >= 0)
            return Qt.rgba(c.r + (1 - c.r) * f, c.g + (1 - c.g) * f, c.b + (1 - c.b) * f, 1)
        return Qt.rgba(c.r * (1 + f), c.g * (1 + f), c.b * (1 + f), 1)
    }

    function groupDigits(digits) {
        var out = ""
        for (var i = 0; i < digits.length; ++i) {
            if (i > 0 && ((digits.length - i) % 3) === 0)
                out += ","
            out += digits.charAt(i)
        }
        return out
    }

    // Natural digits, never scientific notation (44,750,700, not 4.47507E+07).
    function integer(value) {
        var n = Number(value)
        if (!isFinite(n))
            return "—"
        var rounded = Math.round(n)
        return (rounded < 0 ? "-" : "") + groupDigits(Math.abs(rounded).toFixed(0))
    }

    function number(value, maxDecimals) {
        var n = Number(value)
        if (!isFinite(n))
            return "—"
        var decimals = Math.max(0, Math.min(8, Math.floor(Number(maxDecimals) || 0)))
        var fixed = Math.abs(n).toFixed(decimals)
        var dot = fixed.indexOf(".")
        var intPart = dot >= 0 ? fixed.substring(0, dot) : fixed
        var decPart = dot >= 0 ? fixed.substring(dot + 1) : ""
        while (decPart.length > 0 && decPart.charAt(decPart.length - 1) === "0")
            decPart = decPart.substring(0, decPart.length - 1)
        return (n < 0 ? "-" : "") + groupDigits(intPart) + (decPart.length ? "." + decPart : "")
    }

    // 1.20 M, 48.2 K
    function compact(value) {
        var n = Math.max(0, Number(value) || 0)
        if (n >= 1e9) return (n / 1e9).toFixed(2) + " G"
        if (n >= 1e6) return (n / 1e6).toFixed(2) + " M"
        if (n >= 1e4) return (n / 1e3).toFixed(1) + " K"
        return integer(n)
    }

    function pad2(v) { return (v < 10 ? "0" : "") + v }

    // 02:08.41 (or 1:02:08.41)
    function clock(seconds) {
        var s = Math.max(0, Number(seconds) || 0)
        var whole = Math.floor(s)
        var cs = Math.floor((s - whole) * 100)
        var h = Math.floor(whole / 3600), m = Math.floor((whole % 3600) / 60), r = whole % 60
        return (h > 0 ? h + ":" + pad2(m) : pad2(m)) + ":" + pad2(r) + "." + pad2(cs)
    }

    function time(seconds) {
        var s = Math.max(0, Math.floor(Number(seconds) || 0))
        var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), r = s % 60
        return h > 0 ? h + ":" + pad2(m) + ":" + pad2(r) : m + ":" + pad2(r)
    }
}
