pragma Singleton
import QtQuick
import SeabassGui

// Single source of truth for every color and font size this app's own
// custom-drawn UI uses -- no other .qml file should have a literal
// "#xxxxxx" color or a hardcoded font.pixelSize/font.pointSize. Two
// concerns, kept together because both come down to "stop hardcoding
// values the platform or the user's own settings should own":
//
// Colors: when useSystemTheme is false ("Kelp" -- this app's own always-
// dark theme, named after the seabass palette's "Kelp" swatch), every
// color below comes from the fixed kelp* palette and is never sampled
// from Plasma/Material -- so "dark" always actually means dark,
// regardless of what light theme the desktop might be using elsewhere.
// When useSystemTheme is true, colors are derived from Material's own
// resolved palette (materialBackground/materialForeground/etc., pushed in
// by Main.qml via Binding, since only an Item inside the ApplicationWindow's
// tree can read the Material attached properties correctly) so this app's
// custom Rectangles match Material-styled Controls exactly and follow
// Plasma's light/dark choice.
//
// Fonts: every size is a multiple of the *system* font size
// (Qt.application.font.pointSize, which Qt's KDE platform theme
// integration already sets from Plasma's own font settings) rather than
// an absolute pixel/point number, so changing Plasma's font size setting
// resizes this app's text too.
QtObject {
    id: root

    // ---- Pushed in from Main.qml (see its Binding elements) ----
    property bool useSystemTheme: false
    property color materialBackground: "#121212"
    property color materialForeground: "#e0e0e0"
    property color materialDivider: "#33ffffff"

    // ---- "Kelp" -- this app's own always-dark palette ----
    readonly property color kelpBackground: "#14181c"
    readonly property color kelpSurface: "#1a1f24"
    readonly property color kelpBorder: "#333a40"
    readonly property color kelpBorderSubtle: "#2c3238"
    readonly property color kelpText: "#e8ecef"
    readonly property color kelpAccent: "#3daee9"
    readonly property color kelpPrimary: "#123a52"

    // ---- Resolved semantic surface roles -- read only these ----
    readonly property color background: useSystemTheme ? materialBackground : kelpBackground
    readonly property color surface: useSystemTheme ? Qt.tint(materialBackground, Qt.rgba(materialForeground.r, materialForeground.g, materialForeground.b, 0.03)) : kelpSurface
    readonly property color border: useSystemTheme ? materialDivider : kelpBorder
    readonly property color borderSubtle: border
    readonly property color text: useSystemTheme ? materialForeground : kelpText
    readonly property color textMuted: mix(background, text, 0.55)
    // Our own brand colors ("Current"/"Abyss") -- deliberately fixed
    // regardless of useSystemTheme, unlike the structural colors above.
    readonly property color accent: kelpAccent
    readonly property color primary: kelpPrimary

    // Interactive/row-shading states -- always a solid, opaque blend of
    // `surface` toward `text` (never a translucent overlay: compositing a
    // semi-transparent Rectangle over whatever happens to render beneath
    // it is what produced a visibly wrong tint once before). Blending
    // toward `text` rather than always darkening or always lightening is
    // what makes this correct in both directions: `text` is light in dark
    // themes and dark in light themes, so the blend naturally goes the
    // right way in both without special-casing which theme is active.
    readonly property color rowEven: surface
    readonly property color rowOdd: mix(surface, text, 0.045)
    readonly property color rowHover: mix(surface, text, 0.09)
    readonly property color rowPressed: mix(surface, text, 0.16)
    readonly property color groupBackground: mix(surface, background, 0.5)

    // Perceptual luminance of the *actual current* background -- not just
    // useSystemTheme, since useSystemTheme:true resolves to Material.System,
    // which can itself land on a dark Plasma color scheme. Deciding by real
    // luminance is correct in every combination (Kelp, light system theme,
    // dark system theme) instead of two.
    readonly property bool isLightBackground: (background.r * 0.299 + background.g * 0.587 + background.b * 0.114) > 0.5

    // ---- Status colors -- same semantic meaning in both modes (a warning
    // reads as a warning regardless of theme), but NOT the same literal
    // hex in both: the pastel shades below read clearly against Kelp's
    // always-dark background (what these were originally tuned against)
    // but wash out to near-illegible against a light background -- pastel
    // green/red/blue text on near-white has poor contrast even though the
    // hue is "right." Each gets a darker, more saturated light-mode
    // counterpart with the same hue family instead. ----
    readonly property color good: isLightBackground ? "#1e7d32" : "#8fce8f"
    readonly property color info: isLightBackground ? "#1456b0" : "#8ab4f8"
    readonly property color danger: isLightBackground ? "#c62828" : "#ff8080"
    readonly property color warnBg: isLightBackground ? "#fbeecb" : "#4a3510"
    readonly property color warnBorder: isLightBackground ? "#c99a2e" : "#c99a2e"
    readonly property color warnText: isLightBackground ? "#6b4f0a" : "#f0d080"
    readonly property color warnIcon: isLightBackground ? "#a3760a" : "#f0c040"
    readonly property color dangerBg: isLightBackground ? "#f8d7d7" : "#5c1a1a"
    readonly property color dangerBorder: "#e74c3c"
    readonly property color dangerText: isLightBackground ? "#5c1a1a" : "#ffffff"
    readonly property color conflictText: isLightBackground ? "#a85300" : "#ffa500"

    function mix(a, b, t) {
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0);
    }

    // ---- Number formatting shared by pages that talk about sizes and
    // durations (stick backup, statistics). Binary units, one decimal
    // above bytes: "23.4 GiB". ----
    function humanBytes(bytes) {
        if (!bytes || bytes <= 0) return "0 B";
        var units = ["B", "KiB", "MiB", "GiB", "TiB"];
        var value = bytes;
        var unitIndex = 0;
        while (value >= 1024 && unitIndex < units.length - 1) {
            value /= 1024;
            unitIndex++;
        }
        return value.toFixed(unitIndex === 0 ? 0 : 1) + " " + units[unitIndex];
    }

    // "~4 min", "~25 s", "~1 h 12 min"; "" for unknown (< 0).
    function humanDuration(seconds) {
        if (seconds === undefined || seconds === null || seconds < 0) return "";
        if (seconds < 60) return "~" + Math.max(1, Math.round(seconds)) + " s";
        var minutes = Math.round(seconds / 60);
        if (minutes < 60) return "~" + minutes + " min";
        var hours = Math.floor(minutes / 60);
        return "~" + hours + " h " + (minutes % 60) + " min";
    }

    // ---- Musical key -> Camelot wheel color. The wheel itself (12
    // positions arranged by the circle of fifths, each with a relative
    // major/minor pair) is a standard, vendor-neutral DJ convention, not
    // any one company's IP -- but the actual *colors* Mixed In Key/
    // Rekordbox/etc. paint each wedge are their own product design, so
    // this is an original palette, not a reproduction: hue is simply the
    // wheel position itself (camelot number 1..12 spread evenly around
    // the color wheel), so two keys that are close together on the
    // wheel -- i.e. harmonically compatible, safe to mix -- also look
    // close together in color. That visual-distance-mirrors-harmonic-
    // distance property is the actual creative idea here, not a
    // particular set of hex codes. Minor vs. major then reads as
    // "moodier, more saturated and darker" vs. "brighter" at the same
    // hue, rather than a hue change, so relative major/minor pairs
    // (e.g. Am/C, both camelot 8) stay visibly related too. ----

    // Pitch class (0=C .. 11=B) -> Camelot number, one table per mode.
    // Derived directly from the wheel's own relative-major/minor
    // pairing (e.g. camelot 8 is A minor *and* C major), not guessed.
    readonly property var camelotMinorByPitchClass: [5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10]
    readonly property var camelotMajorByPitchClass: [8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1]
    readonly property var pitchClassByName: ({
        "C": 0, "B#": 0,
        "C#": 1, "Db": 1,
        "D": 2,
        "D#": 3, "Eb": 3,
        "E": 4, "Fb": 4,
        "F": 5, "E#": 5,
        "F#": 6, "Gb": 6,
        "G": 7,
        "G#": 8, "Ab": 8,
        "A": 9,
        "A#": 10, "Bb": 10,
        "B": 11, "Cb": 11,
    })
    // Pitch class -> canonical note name, sharps preferred -- only used
    // to synthesize a note name when the input was already Camelot
    // notation (see parseCamelotKey()'s own comment), so "traditional"
    // notation mode has something to display. There's no "right" answer
    // for which enharmonic spelling to pick when working backwards from
    // a Camelot number alone (nothing in "10A" says whether it means
    // B minor or, enharmonically, Cb minor); sharps are the more common
    // convention in DJ software when one has to be chosen.
    readonly property var noteNameByPitchClassSharp:
        ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

    // Parses a key string like "Dm", "F#m", "Bb", "C#" into
    // {camelotNumber: 1..12, isMinor: bool}, or null if it isn't a
    // recognized key spelling (never guessed at -- an unparseable key
    // just gets no color, same "don't fabricate it" stance as
    // conflictText/color-tag handling elsewhere in this codebase).
    //
    // "♯"/"♭" (U+266F/U+266D, the real Unicode music sharp/flat glyphs)
    // are normalized to ASCII "#"/"b" before the lookup -- confirmed via
    // real Engine data that this is not a hypothetical: libdjinterop's
    // own musical_key operator<< (third_party/libdjinterop/include/
    // djinterop/musical_key.hpp) emits these Unicode glyphs for every
    // accidental key ("F♯m", "D♭", ...), so every Engine track whose key
    // isn't a natural note used to fail this lookup and fall back to
    // KeyBadge's plain-text/"unrecognized" display -- not a rare edge
    // case, close to half of any real key distribution given accidentals
    // are as common as naturals.
    // notePart keeps the original ASCII-normalized note+accidental
    // spelling from the input (e.g. "Db" stays "Db", never respelled to
    // "C#") -- only camelotNumber/isMinor collapse enharmonic spellings
    // together. camelotLabel()/colorForKey() only need the latter;
    // traditionalLabel()/traditionalSpokenLabel() below need notePart
    // too, to render the key back out the way it actually reads.
    function parseCamelotKey(keyStr) {
        if (!keyStr || keyStr.length === 0) {
            return null;
        }
        var normalized = keyStr.replace(/♯/g, "#").replace(/♭/g, "b");

        // Camelot notation itself (e.g. "10A", "3B") -- some catalogs
        // store the key field this way already rather than as a musical
        // note name at all (confirmed on real rekordbox DeviceLibrary
        // data: Mixed In Key-style tagging, or a track re-keyed by hand
        // in Camelot terms, both land here as plain text same as any
        // other key). A leading digit is never a valid note-name start,
        // so this can't collide with the musical-notation branch below.
        var camelotMatch = normalized.match(/^(1[0-2]|[1-9])([AB])$/);
        if (camelotMatch) {
            var camelotNum = parseInt(camelotMatch[1], 10);
            var campIsMinor = camelotMatch[2] === "A";
            var byPc = campIsMinor ? camelotMinorByPitchClass : camelotMajorByPitchClass;
            var pc = byPc.indexOf(camelotNum);
            return {camelotNumber: camelotNum, isMinor: campIsMinor, notePart: noteNameByPitchClassSharp[pc]};
        }

        var isMinor = normalized.length > 1 && normalized.charAt(normalized.length - 1) === "m";
        var notePart = isMinor ? normalized.slice(0, -1) : normalized;
        var pitchClass = pitchClassByName[notePart];
        if (pitchClass === undefined) {
            return null;
        }
        var camelotNumber = isMinor ? camelotMinorByPitchClass[pitchClass] : camelotMajorByPitchClass[pitchClass];
        return {camelotNumber: camelotNumber, isMinor: isMinor, notePart: notePart};
    }

    function camelotLabel(keyStr) {
        var parsed = parseCamelotKey(keyStr);
        return parsed ? (parsed.camelotNumber + (parsed.isMinor ? "A" : "B")) : "";
    }

    function colorForKey(keyStr) {
        var parsed = parseCamelotKey(keyStr);
        if (!parsed) {
            return textMuted;
        }
        var hue = (parsed.camelotNumber - 1) / 12;
        return parsed.isMinor ? Qt.hsla(hue, 0.55, 0.38, 1.0) : Qt.hsla(hue, 0.65, 0.55, 1.0);
    }

    // Short badge label in proper musical (not Camelot) notation, e.g.
    // "F♯m", "D♭", "C" -- the real Unicode sharp/flat glyphs, not the
    // ASCII "#"/"b" this app's internal parsing normalizes to (or
    // whatever a given catalog format happens to store; rekordbox and
    // Engine don't even agree with each other -- see parseCamelotKey's
    // own comment on libdjinterop emitting these glyphs natively).
    function traditionalLabel(keyStr) {
        var parsed = parseCamelotKey(keyStr);
        if (!parsed) {
            return "";
        }
        var glyphed = parsed.notePart.replace("#", "♯").replace("b", "♭");
        return glyphed + (parsed.isMinor ? "m" : "");
    }

    // Full spoken form for a tooltip, e.g. "C major", "F♯ minor" -- pairs
    // with traditionalLabel()'s short badge form, spelling out
    // major/minor in full rather than the "m"/(nothing) suffix
    // shorthand, since a tooltip has the room and "how do I actually say
    // this" is the whole point of hovering for detail.
    function traditionalSpokenLabel(keyStr) {
        var parsed = parseCamelotKey(keyStr);
        if (!parsed) {
            return "";
        }
        var noteLetter = parsed.notePart.charAt(0);
        var accidental = parsed.notePart.length > 1 ? parsed.notePart.charAt(1) : "";
        var glyphedNote = noteLetter + accidental.replace("#", "♯").replace("b", "♭");
        return glyphedNote + " " + (parsed.isMinor ? "minor" : "major");
    }

    // Legible text color for a badge painted with colorForKey()'s output
    // -- computed from real luminance rather than assumed from the HSL
    // lightness that produced it, since the same lightness renders
    // visually lighter or darker depending on hue.
    function contrastingTextColor(bg) {
        var luminance = 0.299 * bg.r + 0.587 * bg.g + 0.114 * bg.b;
        return luminance > 0.6 ? "#000000" : "#ffffff";
    }

    // ---- Font scale -- every size a multiple of the system's own font
    // point size, never an absolute number. Sourced from
    // SystemFontMetrics (system_font_metrics.hpp), which reads
    // QFontDatabase::systemFont() -- a direct, live query of the
    // platform theme's current font hint, not QGuiApplication::font()
    // (the app's own currently-set default font, which only reflects
    // the platform theme once something has copied it in there, at
    // timing that isn't guaranteed). A few desktops/build configs report
    // pointSize as -1 (pixel-size-only fonts), so fall back to a
    // conventional default rather than propagate a nonsensical negative
    // multiplier.
    //
    // A previous attempt at this (SystemFontWatcher, reverted -- see git
    // history around commit d32a548) read QGuiApplication::font()
    // through a QEvent::ApplicationFontChange event filter and produced
    // visibly undersized icons/text at startup: that event apparently
    // never fired for a platform-theme-driven change here, so the
    // binding latched onto whatever wrong value QGuiApplication::font()
    // held at first evaluation and nothing ever corrected it.
    // QFontDatabase::systemFont() has no such "has it been copied in
    // yet" gap to begin with -- confirmed via a temporary startup debug
    // print that it already reads correctly at Component.onCompleted --
    // and SystemFontMetrics.changed() (QEvent::ThemeChange, the event
    // for "the platform theme itself changed") keeps it live afterward.
    //
    // smallestReadablePointSize is the platform's own definition of the
    // smallest comfortably-readable UI text size (Plasma's own
    // "smallest font" setting on this platform, backed by whatever
    // QPlatformTheme is active elsewhere -- Windows/macOS included, no
    // platform-specific code needed here) -- a hard floor applied to
    // every font role below that can shrink smaller than the base size.
    // Roles that only ever scale up from the base can't go under the
    // floor once the base can't, so they don't need it applied again.
    readonly property real smallestReadablePointSize: SystemFontMetrics.smallestReadablePointSize > 0
        ? SystemFontMetrics.smallestReadablePointSize : 7
    readonly property real baseFontPointSize: Math.max(smallestReadablePointSize,
        SystemFontMetrics.generalPointSize > 0 ? SystemFontMetrics.generalPointSize : 10)
    readonly property real fontTiny: Math.max(smallestReadablePointSize, baseFontPointSize * 0.85)
    readonly property real fontSmall: Math.max(smallestReadablePointSize, baseFontPointSize * 0.92)
    readonly property real fontNormal: baseFontPointSize
    readonly property real fontMedium: baseFontPointSize * 1.15
    readonly property real fontLarge: baseFontPointSize * 1.4
    readonly property real fontXLarge: baseFontPointSize * 1.6
    readonly property real fontHuge: baseFontPointSize * 2.2

    // ---- Icon sizes -- linked to the same base font size rather than
    // hardcoded pixels, so an icon scales the same way the text next to
    // it does. iconScale is the one deliberate pt-to-px conversion this
    // needs (Item/Layout sizes are pixels, font sizes are points): tuned
    // so today's actual on-screen icon sizes come out unchanged at
    // today's real default base size (confirmed 10pt on this machine),
    // making this a pure refactor with no visual change until someone's
    // system font size actually differs from that default.
    readonly property real iconScale: baseFontPointSize / 10.0
    readonly property real iconSizeSmall: 32 * iconScale
    readonly property real iconSizeNormal: 40 * iconScale
    readonly property real iconSizeLarge: 48 * iconScale

    // ---- spacing scale --------------------------------------------
    //
    // One page, one left edge. These exist because the page they were
    // added for had four different left edges on screen at once -- a
    // header inset by 10, a body by 16, a card's contents by 30, and a
    // breadcrumb whose hover pill pushed its text out by another 8 --
    // and none of the four was a decision anyone had made.
    //
    // pageMargin insets a page's body AND its header, which is what
    // puts the breadcrumb and the first line of content on one vertical
    // line. BackBreadcrumb subtracts crumbTextInset for itself: its
    // segments are hover pills with their own padding, so the row has
    // to start that much further left for the text inside to land on
    // the line.
    //
    // Unscaled on purpose. These are gaps between things, not type, and
    // the pages already in the tree use these same numbers literally --
    // so adopting the tokens moves nothing that was already right.
    readonly property real pageMargin: 16
    readonly property real cardPadding: 16
    readonly property real sectionSpacing: 14   // between blocks down a page
    readonly property real rowSpacing: 10       // between controls across a row
    readonly property real tightSpacing: 6      // a label and the thing it labels
    readonly property real crumbTextInset: 8 * iconScale

    // ---- Titles -- a dedicated (non-bold) display face + scale, set once
    // here and consumed only via PageTitle.qml, so every page title stays
    // consistent. Falls back to the platform's default sans if "Manrope"
    // isn't installed, same as this app already does elsewhere for named
    // fonts (e.g. "Noto Sans Symbols2" for the warning glyph) -- no font
    // is bundled with the app.
    readonly property string titleFamily: "Manrope"
    readonly property int titleWeight: Font.Medium
    readonly property real titleMedium: baseFontPointSize * 2.0
    readonly property real titleLarge: baseFontPointSize * 2.6
    // The breadcrumb row, a step down from titleMedium. That row carries
    // three pieces of text where the rest of a header carries one, and at
    // title size the three of them were what pushed the stick's name into
    // an ellipsis on a window nobody would call narrow. The house glyph
    // that starts the row is deliberately NOT on this step -- an icon
    // reads smaller than text of the same nominal size, and it is the one
    // thing in the row a reader aims a click at.
    readonly property real titleCrumb: baseFontPointSize * 1.7

    // ---- Rest of the type system -- see docs/design/type-scale.html.
    // Subtitle/card title share the title family, just smaller than a
    // page title; table headers and stat values get their own dedicated
    // (mono, for tabular figures) family so numeric columns actually
    // line up. Card titles get their own weight, a step heavier than
    // titleWeight -- at card-title size, Medium next to the card's own
    // muted description text didn't read as distinct enough. ----
    readonly property real subtitleSize: baseFontPointSize * 1.3
    readonly property int cardTitleWeight: Font.DemiBold
    readonly property real cardTitleSize: baseFontPointSize * 1.15
    readonly property string dataFamily: "IBM Plex Mono"
    readonly property real dataSize: baseFontPointSize * 1.8
    readonly property real tableHeaderSize: baseFontPointSize * 0.85

    // ---- Motion -- one shared duration for small, frequent state
    // changes (a row fading in/out of relevance, a hover highlight)
    // rather than each call site picking its own number. Deliberately
    // short: this is for "smooth the edge off an instant change," not a
    // deliberate, noticeable animation -- see CamelotWheelPopup.qml's
    // wedge fade and MatchingPage.qml's row fade for the first
    // uses.
    readonly property int shortTransitionDuration: 120
}
