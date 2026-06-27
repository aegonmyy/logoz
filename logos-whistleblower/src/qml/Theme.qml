import QtQuick

// Design tokens. Instantiated once at the root of Main.qml as `Theme { id: theme }`
// and passed down to components via a `theme` property — same pattern dist-x uses.
QtObject {
    // ── Surfaces (GitHub-dark grade) ─────────────────────────────────────────
    readonly property color bg:            "#0d1117"
    readonly property color surface:       "#161b22"
    readonly property color surfaceSubtle: "#1c2128"
    readonly property color surfaceRaised: "#21262d"

    // ── Borders ──────────────────────────────────────────────────────────────
    readonly property color line:          "#30363d"
    readonly property color lineStrong:    "#444c56"

    // ── Text ─────────────────────────────────────────────────────────────────
    readonly property color fg:            "#e6edf3"
    readonly property color fg2:           "#c9d1d9"
    readonly property color fg3:           "#8b949e"
    readonly property color fg4:           "#6e7681"

    // ── Accent (primary actions / focus rings) ───────────────────────────────
    readonly property color accent:        "#4493f8"
    readonly property color accentHover:   "#58a6ff"
    readonly property color accentMuted:   "#1f6feb"

    // ── Status tones ─────────────────────────────────────────────────────────
    readonly property color success:       "#56d364"
    readonly property color warning:       "#d29922"
    readonly property color danger:        "#f85149"
    readonly property color muted:         "#8b949e"

    // ── Spacing scale ────────────────────────────────────────────────────────
    readonly property int s1: 4
    readonly property int s2: 8
    readonly property int s3: 12
    readonly property int s4: 16
    readonly property int s5: 24
    readonly property int s6: 32

    // ── Radii ────────────────────────────────────────────────────────────────
    readonly property int rSm: 4
    readonly property int rMd: 6
    readonly property int rLg: 10

    // ── Font sizes ───────────────────────────────────────────────────────────
    readonly property int fpHero:  22
    readonly property int fpLg:    16
    readonly property int fpMd:    14
    readonly property int fpBody:  13
    readonly property int fpSm:    12
    readonly property int fpLabel: 11

    // ── Durations ────────────────────────────────────────────────────────────
    readonly property int durFast: 120
    readonly property int durBase: 220
}
