/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

import QtQuick 2.1
import widgets 1.0
import QtQuick.Controls 1.2 as Controls


Controls.ScrollView {
    id: rectangle1


    Item {
        id: canvas

        implicitWidth: childrenRect.width
        implicitHeight: childrenRect.height

        Button {
            y: screenDependHeightDistance
            text: qsTr("New Project")
            anchors.left: sessionsTitle.left
            onClicked: projectWelcomePage.newProject();
            iconSource: "widgets/images/new.png"

        }

        Button {
            y: screenDependHeightDistance
            text: qsTr("Open Project")
            anchors.left: recentProjectsTitle.left
            onClicked: projectWelcomePage.openProject();
            iconSource: "widgets/images/open.png"
        }

        NativeText {
            id: sessionsTitle

            x: 32
            y: screenDependHeightDistance + 77

            color: creatorTheme.Welcome_TextColorHeading
            text: qsTr("Sessions")
            font.pixelSize: 16
            font.family: "Helvetica"
            font.bold: true
        }

        NativeText {
            id: recentProjectsTitle
            x: 406

            y: screenDependHeightDistance + 77
            color: creatorTheme.Welcome_TextColorHeading
            text: qsTr("Recent Projects")
            anchors.left: sessionsTitle.right
            anchors.leftMargin: 280
            font.bold: true
            font.family: "Helvetica"
            font.pixelSize: 16
        }

        RecentProjects {
            x: screenDependLeftMargin

            id: recentProjects

            anchors.leftMargin: 12
            anchors.left: recentProjectsTitle.left

            anchors.top: recentProjectsTitle.bottom
            anchors.topMargin: 20

            model: projectList
        }

        Item {
            id: actions
            x: pageCaption.x + pageCaption.textOffset

            y: screenDependHeightDistance + 244
            width: 140
            height: 70

            anchors.topMargin: 42
            anchors.top: sessions.bottom
        }


        Sessions {
            id: sessions

            x: 96
            y: 144
            width: 274

            anchors.leftMargin: 12
            anchors.left: sessionsTitle.left
            anchors.right: recentProjectsTitle.left
            anchors.rightMargin: 40
            anchors.top: sessionsTitle.bottom
            anchors.topMargin: 20

            model: sessionList
        }
    }
}
