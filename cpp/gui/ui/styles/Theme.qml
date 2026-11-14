pragma Singleton
import QtQuick
import MLC

QtObject {
    
    // HSL-based dynamic theme (mirrors ModSecret.ThemeRefresh exactly)
    // Default: Hue=210, Sat=85, LightAdjust=0
    
    property real hue: 210
    property real sat: 85
    property real lightAdj: 0
    property real hueTopbarDelta: 0

    // HSL -> color helper
    function hsl(h, s, l) {
        return Qt.hsla(h / 360, s / 100, l / 100, 1.0)
    }

    
    // Color 1-8 (exact formulas from ModSecret.vb ThemeRefresh)
    
    readonly property color color1: hsl(hue, sat * 0.2, 25 + lightAdj * 0.3)
    readonly property color color2: "#116ecb"    // global accent blue, exact match original PCL
    readonly property color color3: "#1373fb"    // lighter accent (focus / hover)
    readonly property color color4: "#4895f7"    // light accent (title bar / secondary)
    readonly property color color5: hsl(hue, sat, 80 + lightAdj * 0.4)
    readonly property color color6: hsl(hue, sat * 0.8, 91 + lightAdj * 0.1)
    readonly property color color7: hsl(hue, sat, 95)
    readonly property color color8: hsl(hue, sat, 97)

    // Background gradient colors (PanForm background)
    readonly property color colorBgLeft: hsl(hue - 15, sat * 0.8, 91)
    readonly property color colorBgCenter: hsl(hue, sat * 0.8, 91)
    readonly property color colorBgRight: hsl(hue + 15, sat * 0.8, 91)

    // Derived background (used by controls — scrollbar, combo box, text box)
    readonly property color colorBg0: "#96c0f9"   // color4*0.4 + color5*0.4 + gray4*0.2

    
    // Gray scale
    
    readonly property color gray1: "#404040"
    readonly property color gray2: "#737373"
    readonly property color gray3: "#8c8c8c"
    readonly property color gray4: "#a6a6a6"
    readonly property color gray5: "#cccccc"
    readonly property color gray6: "#ebebeb"
    readonly property color gray7: "#f0f0f0"
    readonly property color gray8: "#f5f5f5"

    
    // Special brushes (matching original StaticResource / DynamicResource)
    
    readonly property color halfWhite: "#55ffffff"       // ColorBrushHalfWhite — 33% white
    readonly property color semiWhite: "#bbffffff"       // ~73% white
    readonly property color pureWhite: "#ffffff"         // Pure white (form background)
    readonly property color semiTransparent: "#01eaf2fe" // ColorBrushSemiTransparent — near-transparent blue
    readonly property color sidebarBg: "#f1ffffff"       // ColorBrushBackgroundTransparentSidebar
    readonly property color redBack: "#80fbdddd"         // ColorBrushRedBack — 50% alpha pink
    readonly property color redLight: "#ff4c4c"          // Red highlight
    readonly property color redDark: "#ce2111"           // Red dark

    
    // Title bar gradient (exact formula from ModSecret.GetTitleBackground)
    // Horizontal: darker edges → lighter center → darker edges
    
    readonly property color titleGradEdge: hsl(hue + (hueTopbarDelta > 0 ? -hueTopbarDelta : 0), sat, 48 + lightAdj)
    readonly property color titleGradCenter: hsl(hue, sat, 54 + lightAdj)
    readonly property color titleGradEnd: hsl(hue + (hueTopbarDelta > 0 ? hueTopbarDelta : 0), sat, 48 + lightAdj)

    
    // Font
    
    property string fontFamily: "Microsoft YaHei UI, PingFang SC, Noto Sans CJK SC, sans-serif"
    readonly property int fontSize: 13
    readonly property int fontSizeSmall: 12
    readonly property int fontSizeXSmall: 11
    readonly property int fontSizeLarge: 14
    readonly property int fontSizeTitle: 15
    readonly property int fontSizeLogo: 18
    readonly property int fontSizeLaunchTitle: 20
    readonly property real fontSizeLaunchLabel: 12.5
    readonly property real fontSizeLaunchName: 13.5

    
    // Window dimensions (exact match FormMain.xaml)
    
    readonly property int windowWidth: 850
    readonly property int windowHeight: 500
    readonly property int windowMinWidth: 810
    readonly property int windowMinHeight: 470
    readonly property int windowCornerRadius: 6
    readonly property int windowMargin: 10       // PanBack margin
    readonly property int borderMargin: 8        // BorderForm margin
    readonly property int titleBarHeight: 50

    
    // Control dimensions
    
    readonly property int buttonRadius: 3
    readonly property int inputRadius: 3
    readonly property int tooltipRadius: 4
    readonly property int menuRadius: 3
    readonly property int scrollbarWidth: 8
    readonly property int scrollbarThumbRadius: 3
    readonly property int launchBtnHeight: 54
    readonly property int versionBtnHeight: 35
    readonly property int folderItemHeight: 40
    readonly property int actionItemHeight: 34
    readonly property int progressBarHeight: 4

    // Title bar
    readonly property int titleBtnSize: 28
    readonly property int titleBtnCloseMargin: 12
    readonly property int titleBtnMinMargin: 44

    // Resizers
    readonly property int resizerThickness: 8
    readonly property int resizerCorner: 13

    // Layout
    readonly property int sidebarWidth: 300
    readonly property int contentMargin: 25
    readonly property int cardPadding: 20
    readonly property int sectionSpacing: 15
    readonly property int itemSpacing: 10
    readonly property int tightSpacing: 5

    // Shadow
    readonly property int shadowWidth: 4
    readonly property real shadowOpacity: 0.04

    // Text opacity
    readonly property real textOpacityDim: 0.5
    readonly property real textOpacityHint: 0.6

    
    // Animation easings (mirroring ModAnimation)
    
    function easeOutBack(t) { var c1 = 1.70158; var c3 = c1 + 1; return 1 + c3 * Math.pow(t - 1, 3) + c1 * Math.pow(t - 1, 2) }
    function easeOutFluent(t) { return 1 - Math.pow(1 - t, 1.8) }
    function easeInFluent(t) { return Math.pow(t, 1.8) }
}
