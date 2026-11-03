// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

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

//! One entry of a class's all-members list.
struct Member {
    //! The bare name, e.g. "config" or "~KConfig" (sorts the list).
    QString name;
    //! As Qt's members lists show it, e.g. "config() const : KSharedConfig::Ptr".
    QString signature;
    //! The member's anchor on its class page.
    QString anchor;
};

//! What a class's all-members page needs from it, also for classes that inherit it.
struct ClassInfo {
    //! Path of the class page, e.g. "kconfigcore/kconfig".
    QString page;
    //! E.g. "KConfig" or "KCoreConfigSkeleton::ItemBool".
    QString name;
    //! Paths of SonicDE base classes, e.g. "kconfigcore/kconfigbase".
    QStringList bases;
    //! Qt base classes: name and the URL of Qt's all-members page.
    QList<std::pair<QString, QString>> qtBases;
    //! Functions and properties, not deprecated ones.
    QList<Member> members;
};

struct Page {
    //! File name without ".xml", e.g. "kconfigwatcher".
    QString slug;
    QString title;
    //! The brief as plain text (the front matter's description).
    QString description;
    QString markdown;
    //! The class's deprecated members, written as "<slug>-obsolete.md"; empty when it has none.
    QString obsoleteMarkdown;
    //! Set on class pages.
    std::optional<ClassInfo> classInfo;
};

//! One section of the reference's contents: a C++ module's classes or a QML module's types.
struct TocSection {
    //! The module page's title and description, e.g. "KConfigCore C++ Classes".
    QString title;
    QString description;
    //! Path of the module page, e.g. "kconfigcore/kconfigcore-module" (empty without one).
    QString page;
    bool qml = false;
    //! Page paths and navigation titles in order, the module page first.
    QList<std::pair<QString, QString>> pages;
};

//! One module's entry in the API reference. It is written next to the module's pages as module.json,
//! so the reference's toc.yaml, index page and all-members pages can be rebuilt from every module
//! converted into the same output directory.
struct ModuleToc {
    QString module;
    //! The C++ section first (when there are C++ pages), then the QML section (when there are QML pages).
    QList<TocSection> sections;
    QList<ClassInfo> classes;

    //! The first section's title (modules are sorted by it).
    QString title() const;

    QByteArray toJson() const;
    //! Reads module.json; returns nothing and sets \a error when it isn't a module entry.
    static std::optional<ModuleToc> fromJson(const QByteArray& json, QString* error);
};

//! Path (without ".md") of the generated page that lists every module: the reference's root page.
inline const QString IndexPage = QStringLiteral("index");

//! The docs plugin's toc.yaml for the whole reference: the index page, then one section per module section.
QString tocYaml(const QList<ModuleToc>& modules);

//! The index page: every C++ and QML module with its description, like Qt's "All Modules" page.
QString indexMarkdown(const QList<ModuleToc>& modules);

//! Every class's all-members page, by path without ".md" (e.g. "kconfigcore/kconfig-members"),
//! including the members of SonicDE base classes from any of \a modules.
QList<std::pair<QString, QString>> membersPages(const QList<ModuleToc>& modules);

/*!
 * Converts QDoc's DocBook output (one article per page) into Markdown for the Winter docs plugin.
 *
 * Class pages get the Qt reference layout: title, brief, requirements, summary sections rebuilt
 * from the member documentation (QDoc's DocBook writer omits them), Detailed Description and member
 * documentation, with explicit heading anchors. Deprecated members move to their own page.
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
