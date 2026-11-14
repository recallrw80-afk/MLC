import QtQuick
import Qt5Compat.GraphicalEffects
import MLC

// Minimal icon display: SVG source + ColorOverlay tinting
Item {
    id: wrapper

    // ---- Public API ----
    property string iconSource: ""     // QRC path (e.g. "qrc:/assets/icons/xxx.svg")
    property string defaultIcon: ""
    property string lucideIcon: ""     // Lucide icon name (e.g. "play") → auto-resolves
    property string assetsIcon: ""
    property color iconColor: Theme.color3
    property int size: 0               // When > 0, overrides width and height

    implicitWidth: size > 0 ? size : 28
    implicitHeight: size > 0 ? size : 28

    readonly property url _resolvedSource: {
        if (defaultIcon !== "")
            return "qrc:/gui/assets/icons/" + defaultIcon + ".svg";
        if (lucideIcon !== "")
            return "qrc:/gui/assets/icons/lucide/" + lucideIcon + ".svg";
        if (assetsIcon !== "")
            return "qrc:/gui/assets/" + assetsIcon + ".svg";
        return iconSource;
    }

    // assets-type icons are transparent by default, lucide gets Theme.color3
    onAssetsIconChanged: if (assetsIcon !== "" && iconColor == Theme.color3)
        iconColor = "transparent"

    Image {
        id: iconImage
        anchors.fill: parent
        source: wrapper._resolvedSource
        sourceSize: Qt.size(wrapper.width * 8, wrapper.height * 8)
        smooth: true
        mipmap: true
        visible: false
    }
    ColorOverlay {
        anchors.fill: iconImage
        source: iconImage
        color: wrapper.iconColor
    }
}
