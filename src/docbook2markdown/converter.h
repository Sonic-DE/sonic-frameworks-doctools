// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace DocBook2Markdown {

struct Options {
    //! Folder name of the module's pages, e.g. "kconfigcore".
    QString module;
    //! Directory holding Qt's module documentation (with its .index files).
    QString qtDocsDir = QStringLiteral("/usr/share/qt6-doc");
    //! Base URL that links into Qt's documentation point to.
    QString qtDocsUrl = QStringLiteral("https://doc.qt.io/qt-6/");
};

struct Page {
    //! File name without ".xml", e.g. "kconfigwatcher".
    QString slug;
    QString title;
    QString markdown;
};

/*!
 * Converts QDoc's DocBook output (one article per page) into Markdown for the Winter docs plugin.
 *
 * Class pages get the Qt reference layout: title, brief, requirements, summary sections rebuilt
 * from the member documentation (QDoc's DocBook writer omits them), Detailed Description and member
 * documentation, with explicit heading anchors.
 */
class Converter {
public:
    explicit Converter(Options options);

    //! Converts one DocBook page; returns nothing and sets \a error when the XML can't be read.
    std::optional<Page> convert(const QString& slug, const QByteArray& xml, QString* error) const;

    //! The docs plugin's toc.yaml for a module's converted pages.
    QString tocYaml(const QList<Page>& pages) const;

    //! Maps a DocBook link target to a Markdown link target.
    QString linkTarget(const QString& href) const;

private:
    Options m_options;
};

}
