// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "converter.h"
#include "kdoctools_version.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <Sonic/Logging/Logging>

#include <algorithm>

SONIC_LOGGING_COMPONENT("org.sonicde.frameworks.doctools.docbook2markdown");

namespace {

bool writeFile(const QString& path, const QString& content)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        sonicCritical() << "Can't write" << path << ":" << file.errorString();
        return false;
    }
    file.write(content.toUtf8());
    if (!file.commit()) {
        sonicCritical() << "Can't write" << path << ":" << file.errorString();
        return false;
    }
    return true;
}

bool copyDirectory(const QString& from, const QString& to)
{
    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QString target = to + QLatin1Char('/') + QDir(from).relativeFilePath(source);
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
            sonicCritical() << "Can't create the directory for" << target;
            return false;
        }
        QFile::remove(target);
        if (!QFile::copy(source, target)) {
            sonicCritical() << "Can't copy" << source << "to" << target;
            return false;
        }
    }
    return true;
}

}

int main(int argc, char** argv)
{
    // Runs in the API docs CI: its errors must also reach stderr when that is a pipe.
    Sonic::Logging::install({.mirrorToStderr = true});

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("docbook2markdown6"));
    QCoreApplication::setApplicationVersion(QStringLiteral(KDOCTOOLS_VERSION_STRING));

    DocBook2Markdown::Options options;

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Converts one module's QDoc DocBook output into Markdown pages for the Winter CMS docs plugin. "
                       "Run it once per module with the same output directory: each run also rewrites toc.yaml, "
                       "index.md and the all-members pages for every module converted there."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption moduleOption(QStringLiteral("module"), QStringLiteral("Folder name for the module's pages, e.g. kconfigcore (required)."), QStringLiteral("name"));
    const QCommandLineOption qtDocsDirOption(QStringLiteral("qt-docs-dir"),
        QStringLiteral("Directory with Qt's module documentation and .index files (default: %1).").arg(options.qtDocsDir),
        QStringLiteral("dir"),
        options.qtDocsDir);
    const QCommandLineOption qtDocsUrlOption(QStringLiteral("qt-docs-url"),
        QStringLiteral("Base URL for links into Qt's documentation (default: %1).").arg(options.qtDocsUrl),
        QStringLiteral("url"),
        options.qtDocsUrl);
    parser.addOptions({moduleOption, qtDocsDirOption, qtDocsUrlOption});
    parser.addPositionalArgument(QStringLiteral("docbook-dir"), QStringLiteral("QDoc's DocBook output directory (one .xml file per page)."));
    parser.addPositionalArgument(QStringLiteral("output-dir"), QStringLiteral("Where to write <module>/<page>.md (classes also get <page>-members.md and, with deprecated members, "
                                                                              "<page>-obsolete.md), <module>/module.json, toc.yaml and index.md."));
    parser.process(app);

    const QStringList positional = parser.positionalArguments();
    if (positional.size() != 2 || !parser.isSet(moduleOption) || parser.value(moduleOption).isEmpty()) {
        parser.showHelp(1);
    }
    options.module = parser.value(moduleOption);
    options.qtDocsDir = parser.value(qtDocsDirOption);
    options.qtDocsUrl = parser.value(qtDocsUrlOption);

    const QDir input(positional.at(0));
    const QString output = positional.at(1);
    if (!input.exists()) {
        sonicCritical() << "The DocBook directory" << input.path() << "doesn't exist";
        return 1;
    }

    // Sorted by page name, so "kconfigskeleton" comes before "kconfigskeleton-itembool".
    QStringList files = input.entryList({QStringLiteral("*.xml")}, QDir::Files);
    std::sort(files.begin(), files.end(), [](const QString& a, const QString& b) {
        return QFileInfo(a).completeBaseName() < QFileInfo(b).completeBaseName();
    });
    if (files.isEmpty()) {
        sonicCritical() << "No .xml pages in" << input.path();
        return 1;
    }

    const QString moduleDir = output + QLatin1Char('/') + options.module;
    if (!QDir().mkpath(moduleDir)) {
        sonicCritical() << "Can't create" << moduleDir;
        return 1;
    }

    const DocBook2Markdown::Converter converter(options);
    QList<DocBook2Markdown::Page> pages;
    for (const QString& name : std::as_const(files)) {
        QFile file(input.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            sonicCritical() << "Can't read" << file.fileName() << ":" << file.errorString();
            return 1;
        }
        QString error;
        const auto page = converter.convert(QFileInfo(name).completeBaseName(), file.readAll(), &error);
        if (!page) {
            sonicCritical() << "Can't convert" << file.fileName() << ":" << error;
            return 1;
        }
        if (!writeFile(moduleDir + QLatin1Char('/') + page->slug + QStringLiteral(".md"), page->markdown)) {
            return 1;
        }
        if (!page->obsoleteMarkdown.isEmpty() && !writeFile(moduleDir + QLatin1Char('/') + page->slug + QStringLiteral("-obsolete.md"), page->obsoleteMarkdown)) {
            return 1;
        }
        pages.append(*page);
    }

    if (input.exists(QStringLiteral("images")) && !copyDirectory(input.filePath(QStringLiteral("images")), moduleDir + QStringLiteral("/images"))) {
        return 1;
    }
    if (!writeFile(moduleDir + QStringLiteral("/module.json"), QString::fromUtf8(converter.moduleToc(pages).toJson()))) {
        return 1;
    }
    sonicInfo() << "Converted" << pages.size() << "pages of module" << options.module << "into" << output;

    // The reference covers every module converted into this directory so far, this one included.
    QList<DocBook2Markdown::ModuleToc> modules;
    for (const QString& name : QDir(output).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        QFile file(output + QLatin1Char('/') + name + QStringLiteral("/module.json"));
        if (!file.exists()) {
            continue;
        }
        QString error;
        const auto module = file.open(QIODevice::ReadOnly) ? DocBook2Markdown::ModuleToc::fromJson(file.readAll(), &error) : std::nullopt;
        if (!module) {
            sonicCritical() << "Can't read" << file.fileName() << ":" << (error.isEmpty() ? file.errorString() : error);
            return 1;
        }
        modules.append(*module);
    }
    // By title, then folder, so the order doesn't depend on the directory listing.
    std::sort(modules.begin(), modules.end(), [](const auto& a, const auto& b) {
        const int byTitle = a.title().compare(b.title(), Qt::CaseInsensitive);
        return byTitle != 0 ? byTitle < 0 : a.module < b.module;
    });
    if (!writeFile(output + QStringLiteral("/toc.yaml"), DocBook2Markdown::tocYaml(modules))
        || !writeFile(output + QLatin1Char('/') + DocBook2Markdown::IndexPage + QStringLiteral(".md"), DocBook2Markdown::indexMarkdown(modules))) {
        return 1;
    }
    // All-members pages include inherited members from other modules, so every module's are rewritten.
    const auto membersPages = DocBook2Markdown::membersPages(modules);
    for (const auto& [path, markdown] : membersPages) {
        if (!writeFile(output + QLatin1Char('/') + path + QStringLiteral(".md"), markdown)) {
            return 1;
        }
    }

    sonicInfo() << "Wrote the reference's toc.yaml, index page and" << membersPages.size() << "all-members pages for" << modules.size() << "modules";
    return 0;
}
