cmake_minimum_required(VERSION 3.20)

project(thumbnailbloom VERSION 0.1.0)

set(QT_MIN_VERSION "6.8.0")
set(KF6_MIN_VERSION "6.5.0")

find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMDeprecationSettings)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core DBus Gui Widgets)
find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS Config ConfigWidgets CoreAddons I18n KCMUtils)
find_package(KWin REQUIRED)

add_subdirectory(src)
