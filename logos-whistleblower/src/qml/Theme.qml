import QtQuick

// Design tokens — dark navy palette with teal accent.
// Used as Theme { id: t } and referenced through that id.
QtObject {
    // ── Canvas ───────────────────────────────────────────────────────────────
    readonly property color canvas:      "#09090f"
    readonly property color panel:       "#111120"
    readonly property color panelMid:    "#18182a"
    readonly property color panelHigh:   "#20203a"

    // ── Borders ──────────────────────────────────────────────────────────────
    readonly property color rim:         "#2a2a4a"
    readonly property color rimStrong:   "#3a3a5c"

    // ── Text ─────────────────────────────────────────────────────────────────
    readonly property color ink:         "#e2e8f0"
    readonly property color ink2:        "#b0bec5"
    readonly property color ink3:        "#78909c"
    readonly property color ink4:        "#546e7a"

    // ── Accent (teal) ────────────────────────────────────────────────────────
    readonly property color teal:        "#14b8a6"
    readonly property color tealHover:   "#2dd4bf"
    readonly property color tealMuted:   "#0d9488"
    readonly property color tealDim:     "#134e4a"

    // ── Status ───────────────────────────────────────────────────────────────
    readonly property color ok:          "#4ade80"
    readonly property color warn:        "#facc15"
    readonly property color err:         "#f87171"
    readonly property color neutral:     "#78909c"

    // ── Spacing scale ────────────────────────────────────────────────────────
    readonly property int sp1: 4
    readonly property int sp2: 8
    readonly property int sp3: 12
    readonly property int sp4: 16
    readonly property int sp5: 24
    readonly property int sp6: 32

    // ── Corner radii ─────────────────────────────────────────────────────────
    readonly property int rXs: 3
    readonly property int rSm: 6
    readonly property int rMd: 10

    // ── Font sizes ───────────────────────────────────────────────────────────
    readonly property int fsTitle: 20
    readonly property int fsLg:    15
    readonly property int fsMd:    13
    readonly property int fsSm:    11
    readonly property int fsXs:    10

    // ── Animation durations ──────────────────────────────────────────────────
    readonly property int quick: 100
    readonly property int smooth: 200
}
