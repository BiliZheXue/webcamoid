/* Webcamoid, camera capture application.
 * Copyright (C) 2024  Gonzalo Exequiel Pedone
 *
 * Webcamoid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Webcamoid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Webcamoid. If not, see <http://www.gnu.org/licenses/>.
 *
 * Web-Site: http://webcamoid.github.io/
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.platform as LABS
import Ak
import AkControls as AK

Page {
    id: colorSchemes

    ScrollView {
        id: scrollView
        anchors.fill: parent
        contentHeight: layout.height
        clip: true

        ColumnLayout {
            id: layout
            width: scrollView.width

            ComboBox {
                id: cbxPalette
                model: AkPalette.availablePalettes()
                Layout.fillWidth: true
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Color scheme")

                Component.onCompleted:
                    currentIndex = model.indexOf(AkPalette.name)

                onCurrentTextChanged: AkPalette.apply(currentText)
            }
            Button {
                text: qsTr("Add")
                flat: true
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Create a new color scheme from the current selected one")

                onClicked: colorSchemeAddEdit.openOptions(false, cbxPalette.currentText)
            }
            Button {
                text: qsTr("Edit")
                flat: true
                enabled: AkPalette.canWrite(cbxPalette.currentText)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Edit the selected color scheme")

                onClicked: colorSchemeAddEdit.openOptions(true, cbxPalette.currentText)
            }
            Button {
                text: qsTr("Remove")
                flat: true
                enabled: AkPalette.canWrite(cbxPalette.currentText)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Delete the selected color scheme")

                onClicked: {
                    AkPalette.remove(cbxPalette.currentText)
                    cbxPalette.model = AkPalette.availablePalettes()
                }
            }
            Button {
                text: qsTr("Import")
                flat: true
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add a new color scheme from a file")

                onClicked: importColorSchemeDialog.open()
            }
            Button {
                text: qsTr("Export")
                flat: true
                ToolTip.text: qsTr("Save the selected color scheme into a file")

                onClicked: {
                    exportColorSchemeDialog.file = cbxPalette.currentText + ".colors.conf"
                    exportColorSchemeDialog.open()
                }
            }
            TabBar {
                id: tabBar
                Layout.fillWidth: true

                TabButton {
                    text: qsTr("Enabled")
                }
                TabButton {
                    text: qsTr("Disabled")
                }
            }
            GroupBox {
                title: qsTr("Preview")
                Layout.fillWidth: true

                Frame {
                    anchors.fill: parent
                    enabled: glyControls.controlEnabled

                    ColumnLayout {
                        id: glyControls
                        anchors.fill: parent

                        property bool controlEnabled: tabBar.currentIndex < 1
                        property int controlValue: 50
                        property int controlMinValue: 0
                        property int controlMaxValue: 100
                        property int controlStep: 1

                        RowLayout {
                            Label {
                                text: qsTr("Window text <a href=\"#\">link</a>")
                                enabled: glyControls.controlEnabled
                                Layout.fillWidth: true
                            }
                            Button {
                                id: btnExampleButton
                                text: qsTr("Button text")
                                enabled: glyControls.controlEnabled
                            }
                            ToolTip {
                                id: toolTip
                                text: qsTr("Tooltip text")
                                visible: btnExampleButton.hovered && glyControls.controlEnabled
                            }
                        }
                        RowLayout {
                            CheckBox {
                                enabled: glyControls.controlEnabled
                            }
                            TextField {
                                placeholderText: qsTr("Placeholder text")
                                text: qsTr("Text")
                                enabled: glyControls.controlEnabled
                                Layout.fillWidth: true
                            }
                            Switch {
                                enabled: glyControls.controlEnabled
                            }
                        }
                        RowLayout {
                            Slider {
                                value: glyControls.controlValue
                                from: glyControls.controlMinValue
                                to: glyControls.controlMaxValue
                                stepSize: glyControls.controlStep
                                enabled: glyControls.controlEnabled
                                Layout.fillWidth: true

                                onValueChanged: glyControls.controlValue = value
                            }
                            SpinBox {
                                value: glyControls.controlValue
                                from: glyControls.controlMinValue
                                to: glyControls.controlMaxValue
                                stepSize: glyControls.controlStep
                                enabled: glyControls.controlEnabled

                                onValueChanged: glyControls.controlValue = value
                            }
                        }
                        Label {
                            text: qsTr("Progress %1%").arg(glyControls.controlValue)
                            enabled: glyControls.controlEnabled
                            Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
                        }
                        ProgressBar {
                            value: glyControls.controlValue
                            from: glyControls.controlMinValue
                            to: glyControls.controlMaxValue
                            enabled: glyControls.controlEnabled
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }
    ColorSchemeAddEdit {
        id: colorSchemeAddEdit
        width: colorSchemes.Window.width
        height: colorSchemes.Window.height
        anchors.centerIn: Overlay.overlay

        onPaletteUpdated: function (paletteName) {
            cbxPalette.model = AkPalette.availablePalettes()
        }
    }
    LABS.FileDialog {
        id: importColorSchemeDialog
        title: qsTr("Select the color scheme to import")
        fileMode: LABS.FileDialog.OpenFiles
        selectedNameFilter.index: 0
        nameFilters: ["Color scheme file (*.colors.conf)"]

        onAccepted: {
            let selectedFiles = importColorSchemeDialog.files
            let selectedPalette = ""
            let loadedPalettes = 0

            for (let i in selectedFiles) {
                let palName = AkPalette.loadFromFileName(mediaTools.urlToLocalFile(selectedFiles[i]))

                if (palName.length < 1)
                    continue

                AkPalette.save(palName)

                if (selectedPalette.length < 1)
                    selectedPalette = palName

                loadedPalettes++
            }

            cbxPalette.model = AkPalette.availablePalettes()

            if (loadedPalettes == 1 && selectedPalette.length > 0) {
                cbxPalette.currentIndex = cbxPalette.model.indexOf(selectedPalette)
                AkPalette.apply(selectedPalette)
            }
        }
    }
    LABS.FileDialog {
        id: exportColorSchemeDialog
        title: qsTr("Save the color scheme to a file")
        fileMode: LABS.FileDialog.SaveFile
        file: cbxPalette.currentText + ".colors.conf"
        selectedNameFilter.index: 0
        nameFilters: ["Color scheme file (*.colors.conf)"]

        onAccepted:
            AkPalette.saveToFileName(mediaTools.urlToLocalFile(exportColorSchemeDialog.file),
                                     cbxPalette.currentText);
    }
}
