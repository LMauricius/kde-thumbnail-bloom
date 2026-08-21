# The effect plugin itself.
kcoreaddons_add_plugin(thumbnailbloom
    SOURCES
        bloomlayout.cpp
        thumbnailbend.cpp
        thumbnailbloom.cpp
        thumbnailoverlay.cpp
    INSTALL_NAMESPACE "kwin/effects/plugins"
)
kconfig_add_kcfg_files(thumbnailbloom thumbnailbloomconfig.kcfgc)
target_link_libraries(thumbnailbloom PRIVATE
    KWin::kwin
    Qt6::Core
    Qt6::Gui
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::ColorScheme
    KF6::CoreAddons
    KF6::I18n
)

# The settings module shown by System Settings -> Desktop Effects.
kcoreaddons_add_plugin(kwin_thumbnailbloom_config
    SOURCES thumbnailbloom_config.cpp
    INSTALL_NAMESPACE "kwin/effects/configs"
)
ki18n_wrap_ui(kwin_thumbnailbloom_config thumbnailbloom_config.ui)
kconfig_add_kcfg_files(kwin_thumbnailbloom_config thumbnailbloomconfig.kcfgc)
target_link_libraries(kwin_thumbnailbloom_config PRIVATE
    Qt6::Widgets
    KF6::ConfigWidgets
    KF6::CoreAddons
    KF6::I18n
    KF6::KCMUtils
    Qt6::DBus
)
