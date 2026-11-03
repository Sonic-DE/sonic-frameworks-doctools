// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>
#include <utility>

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
    //! The brief as plain text (the front matter's description).
    QString description;
    QString markdown;
};

//! One module's entry in the API reference. It is written next to the module's pages as module.json,
//! so the reference's toc.yaml and index page can be rebuilt from every module converted into the
//! same output directory.
struct ModuleToc {
    QString module;
    //! The module page's title and description, e.g. "KConfigCore C++ Classes".
    QString title;
    QString description;
    //! Path of the module page, e.g. "kconfigcore/kconfigcore-module" (empty without one).
    QString modulePage;
    //! Page paths and navigation titles in order: the module page, classes, then other pages.
    QList<std::pair<QString, QString>> pages;

    QByteArray toJson() const;
    //! Reads module.json; returns nothing and sets \a error when it isn't a module entry.
    static std::optional<ModuleToc> fromJson(const QByteArray& json, QString* error);
};

//! Path (without ".md") of the generated page that lists every module: the reference's root page.
inline const QString IndexPage = QStringLiteral("index");

//! The docs plugin's toc.yaml for the whole reference: the index page, then one section per module.
QString tocYaml(const QList<ModuleToc>& modules);

//! The index page: every module with its description, like Qt's "All Modules" page.
QString indexMarkdown(const QList<ModuleToc>& modules);

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

    //! The module's entry in the reference, from its converted pages.
    ModuleToc moduleToc(const QList<Page>& pages) const;

    //! Maps a DocBook link target to a Markdown link target.
    QString linkTarget(const QString& href) const;

private:
    Options m_options;
};

}
