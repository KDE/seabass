import QtQuick
import QtQuick.Controls
import SeabassGui

// Shared page/section title -- see docs/design/type-scale.html for the
// typography decision this implements ("Quiet" pairing). Replaces the
// font.bold/font.pointSize pair that used to be repeated in every page
// header. Titles are never bold; "page" is for the single top-level
// screen (StickListPage), "section" is every page reached via the back
// chevron.
Label {
    id: root
    property string level: "section"  // "page" | "section" | "crumb"

    font.family: Theme.titleFamily
    font.weight: Theme.titleWeight
    font.pointSize: root.level === "page" ? Theme.titleLarge
        : root.level === "crumb" ? Theme.titleCrumb
        : Theme.titleMedium
    elide: Text.ElideRight
}
