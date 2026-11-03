// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "converter.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTest>

#include <Sonic/Logging/Log>

SONIC_LOGGING_COMPONENT("org.sonicde.frameworks.doctools.docbook2markdown.test");

using namespace DocBook2Markdown;

class DocBook2MarkdownTest : public QObject {
    Q_OBJECT

private:
    Converter converter() const
    {
        Options options;
        options.module = QStringLiteral("testmodule");
        options.qtDocsDir = QStringLiteral(DATA_DIR "/qtdocs");
        return Converter(options);
    }

    QList<Page> allPages() const
    {
        QList<Page> pages;
        const QDir dir(QStringLiteral(DATA_DIR "/docbook"));
        for (const QString& name : dir.entryList({QStringLiteral("*.xml")}, QDir::Files, QDir::Name)) {
            QFile file(dir.filePath(name));
            if (!file.open(QIODevice::ReadOnly)) {
                qFatal("can't read fixture %s", qPrintable(file.fileName()));
            }
            QString error;
            const auto page = converter().convert(name.chopped(4), file.readAll(), &error);
            if (!page) {
                qFatal("fixture %s didn't convert: %s", qPrintable(name), qPrintable(error));
            }
            pages.append(*page);
        }
        return pages;
    }

    Page page(const QString& slug) const
    {
        for (const Page& p : allPages()) {
            if (p.slug == slug) {
                return p;
            }
        }
        qFatal("no fixture page %s", qPrintable(slug));
    }

    //! The lines of a "## <title>" section, up to the next "## " heading.
    static QStringList sectionLines(const QString& markdown, const QString& title)
    {
        QStringList result;
        bool inside = false;
        for (const QString& line : markdown.split(QLatin1Char('\n'))) {
            if (line.startsWith(QLatin1String("## "))) {
                inside = line == QStringLiteral("## ") + title;
                continue;
            }
            if (inside && !line.isEmpty()) {
                result.append(line);
            }
        }
        return result;
    }

private Q_SLOTS:
    void everySummaryEntryLinksToAnAnchorOnThePage()
    {
        for (const Page& p : allPages()) {
            static const QRegularExpression anchors(QStringLiteral(R"(\{#([^}]+)\})"));
            QSet<QString> ids;
            auto found = anchors.globalMatch(p.markdown);
            while (found.hasNext()) {
                ids.insert(found.next().captured(1));
            }
            static const QRegularExpression summaryLink(QStringLiteral(R"(^- \[.*\]\(#([^)]+)\))"), QRegularExpression::MultilineOption);
            auto links = summaryLink.globalMatch(p.markdown);
            while (links.hasNext()) {
                const QString target = links.next().captured(1);
                QVERIFY2(ids.contains(target), qPrintable(p.slug + QStringLiteral(": no anchor for #") + target));
            }
        }
    }

    void memberTagsChooseTheSummarySection()
    {
        const QString markdown = page(QStringLiteral("testwatcher")).markdown;
        QVERIFY(sectionLines(markdown, QStringLiteral("Signals")).join(QLatin1Char('\n')).contains(QLatin1String("(#changed)")));
        QVERIFY(sectionLines(markdown, QStringLiteral("Public Slots")).join(QLatin1Char('\n')).contains(QLatin1String("(#reload)")));
        QVERIFY(sectionLines(markdown, QStringLiteral("Static Public Members")).join(QLatin1Char('\n')).contains(QLatin1String("(#create)")));
        QVERIFY(sectionLines(markdown, QStringLiteral("Properties")).join(QLatin1Char('\n')).contains(QLatin1String("(#interval-prop)")));
        QVERIFY(sectionLines(markdown, QStringLiteral("Public Types")).join(QLatin1Char('\n')).contains(QLatin1String("(#Mode-enum)")));
        const QString functions = sectionLines(markdown, QStringLiteral("Public Functions")).join(QLatin1Char('\n'));
        QVERIFY(functions.contains(QLatin1String("(#config)")));
        QVERIFY(!functions.contains(QLatin1String("(#changed)")));
    }

    void summaryEntriesDropTagsAndQualifiersAndShowSince()
    {
        const QString markdown = page(QStringLiteral("testwatcher")).markdown;
        for (const QString& title : {QStringLiteral("Public Functions"), QStringLiteral("Signals"), QStringLiteral("Public Slots"), QStringLiteral("Static Public Members")}) {
            for (const QString& line : sectionLines(markdown, title)) {
                QVERIFY2(!line.contains(QLatin1String("\\[")), qPrintable(line));
                QVERIFY2(!line.contains(QLatin1String("TestWatcher::")), qPrintable(line));
            }
        }
        QVERIFY(markdown.contains(QLatin1String("(#TestWatcher) (since 6.23)")));
    }

    void linksMapToSiteAndQtUrls()
    {
        Converter c = converter();
        QCOMPARE(c.linkTarget(QStringLiteral("../qtcore/qobject.html#connect")), QStringLiteral("https://doc.qt.io/qt-6/qobject.html#connect"));
        QCOMPARE(c.linkTarget(QStringLiteral("../otherframework/othergroup.html#name")), QStringLiteral("../otherframework/othergroup.md#name"));
        QCOMPARE(c.linkTarget(QStringLiteral("testconfig.xml")), QStringLiteral("testconfig.md"));
        QCOMPARE(c.linkTarget(QStringLiteral("testwatcher.xml#changed")), QStringLiteral("testwatcher.md#changed"));
        QCOMPARE(c.linkTarget(QStringLiteral("https://example.org/spec")), QStringLiteral("https://example.org/spec"));

        for (const Page& p : allPages()) {
            QVERIFY(!p.markdown.contains(QLatin1String(".xml")));
            // No link without a target: QDoc's unresolved links (empty href) become plain text.
            QVERIFY2(!p.markdown.contains(QLatin1String("]()")) && !p.markdown.contains(QLatin1String("](#)")), qPrintable(p.slug));
        }
        QVERIFY(page(QStringLiteral("testwatcher")).markdown.contains(QLatin1String("This function is Q\\_INVOKABLE.")));
    }

    void requirementsHaveNoEmptyRowsAndAnInclude()
    {
        const QString markdown = page(QStringLiteral("testwatcher")).markdown;
        QVERIFY(markdown.contains(QLatin1String("- **Header:** `#include <TestWatcher>`")));
        QVERIFY(markdown.contains(QLatin1String("- **Inherits:** [QObject](https://doc.qt.io/qt-6/qobject.html)")));
        static const QRegularExpression emptyRow(QStringLiteral(R"(^- \*\*[^*]+:\*\*\s*$)"), QRegularExpression::MultilineOption);
        QVERIFY(!emptyRow.match(markdown).hasMatch());
        QVERIFY(!markdown.contains(QLatin1String("Inherited By")));
    }

    void seeAlsoIsAParagraphAndEmphasisHasNoInnerEdgeSpaces()
    {
        for (const Page& p : allPages()) {
            QVERIFY(!p.markdown.contains(QLatin1String("# See Also")));
            static const QRegularExpression badEmphasis(QStringLiteral(R"((^|[^*\\])\*(\s[^*]*|[^*]*\s)\*([^*]|$))"), QRegularExpression::MultilineOption);
            for (const QString& line : p.markdown.split(QLatin1Char('\n'))) {
                if (line.startsWith(QLatin1String("- ")) || line.startsWith(QLatin1String("#"))) {
                    continue;
                }
                QVERIFY2(!badEmphasis.match(line).hasMatch(), qPrintable(p.slug + QStringLiteral(": ") + line));
            }
        }
        QVERIFY(page(QStringLiteral("testwatcher")).markdown.contains(QLatin1String("*See also* [TestConfig](testconfig.md), [the specification](https://example.org/spec)")));
    }

    void qdocBrandingIsDropped()
    {
        for (const Page& p : allPages()) {
            QVERIFY(!p.markdown.contains(QLatin1String("Reference Documentation")));
        }
    }

    void tableCellsKeepBareTextAndCode()
    {
        const QString markdown = page(QStringLiteral("testwatcher")).markdown;
        QVERIFY(markdown.contains(QLatin1String("| Constant | Value | Description |")));
        QVERIFY(markdown.contains(QLatin1String("| `TestWatcher::Polling` | `0x01` | Poll the file. |")));
        QVERIFY(markdown.contains(QLatin1String("**flags TestWatcher::Modes**")));
    }

    void memberHeadingsKeepExplicitAnchors()
    {
        const QString markdown = page(QStringLiteral("testwatcher")).markdown;
        for (const QString& id : {QStringLiteral("details"), QStringLiteral("changed"), QStringLiteral("create"), QStringLiteral("Mode-enum")}) {
            QVERIFY2(markdown.contains(QStringLiteral(" {#%1}\n").arg(id)), qPrintable(id));
        }
    }

    void frontMatterHasTitleAndPlainDescription()
    {
        const QString markdown = page(QStringLiteral("testwatcher")).markdown;
        QVERIFY(markdown.startsWith(QLatin1String("---\ntitle: \"TestWatcher Class\"\ndescription: \"Watches a TestConfig for changes.\"\n---\n")));
    }

    void tocListsEveryClassWithTheModulePageAsRoot()
    {
        const QList<Page> pages = allPages();
        const QString toc = converter().tocYaml(pages);
        QVERIFY(toc.startsWith(QLatin1String("rootPage: testmodule/testmodule-module\n")));
        for (const Page& p : pages) {
            QVERIFY2(toc.contains(QStringLiteral("testmodule/%1:").arg(p.slug)), qPrintable(p.slug));
        }
    }

    void invalidXmlIsAnError()
    {
        QString error;
        QVERIFY(!converter().convert(QStringLiteral("broken"), QByteArrayLiteral("<db:article xmlns:db=\"http://docbook.org/ns/docbook\"><db:info>"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!converter().convert(QStringLiteral("html"), QByteArrayLiteral("<html><body/></html>"), &error));
        QVERIFY(error.contains(QLatin1String("not a DocBook document")));
    }
};

QTEST_GUILESS_MAIN(DocBook2MarkdownTest)

#include "docbook2markdowntest.moc"
