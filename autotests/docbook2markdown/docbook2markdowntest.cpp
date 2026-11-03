// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "converter.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTest>

#include <Sonic/Logging/Log>

#include <algorithm>

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

    static QSet<QString> anchorsOf(const QString& markdown)
    {
        static const QRegularExpression anchors(QStringLiteral(R"(\{#([^}]+)\})"));
        QSet<QString> ids;
        auto found = anchors.globalMatch(markdown);
        while (found.hasNext()) {
            ids.insert(found.next().captured(1));
        }
        return ids;
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
            for (const QString& markdown : {p.markdown, p.obsoleteMarkdown}) {
                const QSet<QString> ids = anchorsOf(markdown);
                static const QRegularExpression summaryLink(QStringLiteral(R"(^- \[.*\]\(#([^)]+)\))"), QRegularExpression::MultilineOption);
                auto links = summaryLink.globalMatch(markdown);
                while (links.hasNext()) {
                    const QString target = links.next().captured(1);
                    QVERIFY2(ids.contains(target), qPrintable(p.slug + QStringLiteral(": no anchor for #") + target));
                }
            }
        }
    }

    void deprecatedMembersHaveTheirOwnPage()
    {
        const Page watcher = page(QStringLiteral("testwatcher"));
        const QString obsolete = watcher.obsoleteMarkdown;
        QVERIFY(obsolete.contains(QLatin1String("\n# Obsolete Members for TestWatcher\n")));
        QVERIFY(obsolete.contains(QLatin1String("are deprecated.")));
        QVERIFY(obsolete.contains(QLatin1String(" {#reset}\n")));
        QVERIFY(obsolete.contains(QLatin1String("(#reset) (deprecated in 6.3)")));
        QVERIFY(obsolete.contains(QLatin1String("parent: \"testmodule/testwatcher\"")));
        // The class page keeps none of it, links to it, and its links to deprecated members follow them.
        QVERIFY(!watcher.markdown.contains(QLatin1String("{#reset}")));
        QVERIFY(!watcher.markdown.contains(QLatin1String("Obsolete")));
        QVERIFY(watcher.markdown.contains(QLatin1String("[Deprecated members](testwatcher-obsolete.md)")));
        QVERIFY(watcher.markdown.contains(QLatin1String("[reset](testwatcher-obsolete.md#reset)")));
        // Links from the obsolete page to current members stay on the class page.
        QVERIFY(obsolete.contains(QLatin1String("[reload](testwatcher.md#reload)")));

        const Page base = page(QStringLiteral("testbase"));
        QVERIFY(base.obsoleteMarkdown.isEmpty());
        QVERIFY(!base.markdown.contains(QLatin1String("Deprecated members")));
    }

    void classPagesLinkTheirMembersPage()
    {
        for (const Page& p : allPages()) {
            const bool links = p.markdown.contains(QStringLiteral("](%1-members.md)").arg(p.slug));
            QCOMPARE(links, p.classInfo.has_value());
        }
    }

    void membersPagesListOwnAndInheritedMembers()
    {
        const ModuleToc toc = converter().moduleToc(allPages());
        const auto pages = membersPages({toc});
        QCOMPARE(pages.size(), toc.classes.size());

        QHash<QString, QString> byPath;
        for (const auto& [path, markdown] : pages) {
            byPath.insert(path, markdown);
        }
        // Every member of the class and of its SonicDE bases is listed, linked to its anchor on its page.
        for (const ClassInfo& info : toc.classes) {
            const QString markdown = byPath.value(info.page + QStringLiteral("-members"));
            QVERIFY2(!markdown.isEmpty(), qPrintable(info.page));
            QVERIFY(markdown.contains(QStringLiteral("parent: \"%1\"").arg(info.page)));
            QList<const ClassInfo*> chain{&info};
            for (const QString& base : info.bases) {
                for (const ClassInfo& other : toc.classes) {
                    if (other.page == base) {
                        chain.append(&other);
                    }
                }
            }
            for (const ClassInfo* owner : std::as_const(chain)) {
                const Page ownerPage = page(owner->page.section(QLatin1Char('/'), 1));
                const QSet<QString> anchors = anchorsOf(ownerPage.markdown);
                const QString ownerName = owner->name.section(QStringLiteral("::"), -1);
                for (const Member& member : owner->members) {
                    QVERIFY2(anchors.contains(member.anchor), qPrintable(ownerPage.slug + QStringLiteral(": no anchor for #") + member.anchor));
                    // A base's constructors and destructor aren't inherited.
                    const bool special = member.name == ownerName || member.name == QLatin1Char('~') + ownerName;
                    const bool listed = markdown.contains(QStringLiteral("](%1.md#%2)").arg(ownerPage.slug, member.anchor));
                    QVERIFY2(listed == (owner == &info || !special), qPrintable(info.name + QStringLiteral(": ") + member.signature));
                }
            }
        }

        const QString watcher = byPath.value(QStringLiteral("testmodule/testwatcher-members"));
        QVERIFY(watcher.contains(QLatin1String("](testbase.md#entries)")));
        // Qt bases, also through SonicDE ones, are linked; deprecated members aren't listed.
        QVERIFY(watcher.contains(QLatin1String("[QObject](https://doc.qt.io/qt-6/qobject-members.html)")));
        QVERIFY(!watcher.contains(QLatin1String("#reset)")));
        // The constructor comes first.
        static const QRegularExpression firstEntry(QStringLiteral(R"(^- \[([^\]]*)\])"), QRegularExpression::MultilineOption);
        QVERIFY(firstEntry.match(watcher).captured(1).startsWith(QLatin1String("TestWatcher(")));
    }

    void memberSignaturesLookLikeQtsLists()
    {
        const ModuleToc toc = converter().moduleToc(allPages());
        QHash<QString, QString> signatures;
        for (const ClassInfo& info : toc.classes) {
            for (const Member& member : info.members) {
                signatures.insert(member.anchor, member.signature);
            }
        }
        // No parameter names, default arguments, tags, qualifiers or "void"; return types after a colon.
        QCOMPARE(signatures.value(QStringLiteral("changed")), QStringLiteral("changed(const QByteArrayList &)"));
        QCOMPARE(signatures.value(QStringLiteral("config")), QStringLiteral("config() const : TestConfig"));
        QCOMPARE(signatures.value(QStringLiteral("create")), QStringLiteral("create(const TestConfig &) : TestWatcher *"));
        QCOMPARE(signatures.value(QStringLiteral("entries")), QStringLiteral("entries(const QStringList &, const QSize &) const : QMap<QString, QVariant>"));
        QCOMPARE(signatures.value(QStringLiteral("operator-eq-eq")), QStringLiteral("operator==(const TestBase &) const : bool"));
        QCOMPARE(signatures.value(QStringLiteral("dtor.TestBase")), QStringLiteral("~TestBase()"));
        QCOMPARE(signatures.value(QStringLiteral("interval-prop")), QStringLiteral("interval : int"));
        // A nested class's members are qualified only with the inner class name.
        QCOMPARE(signatures.value(QStringLiteral("Item")), QStringLiteral("Item(bool &, bool)"));

        // So its constructor still comes first in its list.
        for (const auto& [path, markdown] : membersPages({toc})) {
            if (path == QLatin1String("testmodule/testbase-item-members")) {
                static const QRegularExpression firstEntry(QStringLiteral(R"(^- \[([^\]]*)\])"), QRegularExpression::MultilineOption);
                QCOMPARE(firstEntry.match(markdown).captured(1), QStringLiteral("Item(bool &, bool)"));
            }
        }
    }

    void qmlPagesFormTheirOwnSection()
    {
        const ModuleToc toc = converter().moduleToc(allPages());
        QCOMPARE(toc.sections.size(), 2);
        const TocSection& cpp = toc.sections.at(0);
        const TocSection& qml = toc.sections.at(1);
        QVERIFY(!cpp.qml);
        QVERIFY(qml.qml);
        QCOMPARE(qml.page, QStringLiteral("testmodule/org-kde-test-qmlmodule"));
        QCOMPARE(qml.title, QStringLiteral("Test QML Types"));
        for (const auto& [path, title] : cpp.pages) {
            QVERIFY2(!path.contains(QLatin1String("/qml-")) && !path.endsWith(QLatin1String("-qmlmodule")), qPrintable(path));
        }
        QVERIFY(qml.pages.contains(std::pair(QStringLiteral("testmodule/qml-org-kde-test-testitem"), QStringLiteral("TestItem"))));

        const QString index = indexMarkdown({toc});
        QVERIFY(index.indexOf(QLatin1String("## C++ Modules")) < index.indexOf(QLatin1String("## QML Modules")));
        QVERIFY(index.contains(QLatin1String("- [Test QML Types](testmodule/org-kde-test-qmlmodule.md): Types for testing in QML.")));
    }

    void moreLinksOnlyWhereThereIsADetailedDescription()
    {
        for (const Page& p : allPages()) {
            const bool hasDetails = p.markdown.contains(QLatin1String("{#details}\n"));
            QCOMPARE(p.markdown.contains(QLatin1String("[More...](#details)")), hasDetails);
        }
        // The QML module page's "details" is a bare anchor, not a Detailed Description.
        QVERIFY(!page(QStringLiteral("org-kde-test-qmlmodule")).markdown.contains(QLatin1String("More...")));
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
        QVERIFY(markdown.contains(QLatin1String("- **Inherits:** [TestBase](testbase.md)")));
        QVERIFY(page(QStringLiteral("testbase")).markdown.contains(QLatin1String("- **Inherits:** [QObject](https://doc.qt.io/qt-6/qobject.html)")));
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

    void moduleTocListsEveryPageOnceWithTheModulePagesFirst()
    {
        const QList<Page> pages = allPages();
        const ModuleToc toc = converter().moduleToc(pages);
        QStringList paths;
        for (const TocSection& section : toc.sections) {
            QVERIFY(!section.description.isEmpty());
            QCOMPARE(section.pages.first().first, section.page);
            for (const auto& [path, title] : section.pages) {
                paths.append(path);
            }
        }
        QCOMPARE(paths.size(), pages.size());
        for (const Page& p : pages) {
            QVERIFY2(paths.contains(QStringLiteral("testmodule/") + p.slug), qPrintable(p.slug));
        }
        QVERIFY(toc.sections.first().page.endsWith(QLatin1String("-module")));
    }

    void moduleTocSurvivesItsJson()
    {
        const ModuleToc toc = converter().moduleToc(allPages());
        QString error;
        const auto read = ModuleToc::fromJson(toc.toJson(), &error);
        QVERIFY2(read, qPrintable(error));
        QCOMPARE(read->module, toc.module);
        QCOMPARE(read->sections.size(), toc.sections.size());
        for (qsizetype i = 0; i < toc.sections.size(); ++i) {
            QCOMPARE(read->sections.at(i).title, toc.sections.at(i).title);
            QCOMPARE(read->sections.at(i).description, toc.sections.at(i).description);
            QCOMPARE(read->sections.at(i).page, toc.sections.at(i).page);
            QCOMPARE(read->sections.at(i).qml, toc.sections.at(i).qml);
            QCOMPARE(read->sections.at(i).pages, toc.sections.at(i).pages);
        }
        QCOMPARE(read->classes.size(), toc.classes.size());
        for (qsizetype i = 0; i < toc.classes.size(); ++i) {
            const ClassInfo& a = read->classes.at(i);
            const ClassInfo& b = toc.classes.at(i);
            QCOMPARE(a.page, b.page);
            QCOMPARE(a.name, b.name);
            QCOMPARE(a.bases, b.bases);
            QCOMPARE(a.qtBases, b.qtBases);
            QCOMPARE(a.members.size(), b.members.size());
            for (qsizetype m = 0; m < b.members.size(); ++m) {
                QCOMPARE(a.members.at(m).name, b.members.at(m).name);
                QCOMPARE(a.members.at(m).signature, b.members.at(m).signature);
                QCOMPARE(a.members.at(m).anchor, b.members.at(m).anchor);
            }
        }

        QVERIFY(!ModuleToc::fromJson(QByteArrayLiteral("{\"module\": \"x\"}"), &error));
        QVERIFY(!ModuleToc::fromJson(QByteArrayLiteral("not json"), &error));
    }

    void referenceTocHasTheIndexAsRootAndUniqueModuleSections()
    {
        // Two modules with the same pages and the same titles, under different folders.
        const ModuleToc first = converter().moduleToc(allPages());
        ModuleToc second = first;
        second.module = QStringLiteral("othermodule");
        for (TocSection& section : second.sections) {
            for (auto& entry : section.pages) {
                entry.first.replace(QLatin1String("testmodule/"), QLatin1String("othermodule/"));
            }
        }
        const QString toc = tocYaml({first, second});

        QVERIFY(toc.startsWith(QStringLiteral("rootPage: %1\n").arg(IndexPage)));
        QVERIFY(toc.contains(QStringLiteral("      %1: ").arg(IndexPage)));
        // Section titles are YAML keys at two spaces of indentation: none may repeat.
        QStringList sections;
        for (const QString& line : toc.split(QLatin1Char('\n'))) {
            if (line.startsWith(QLatin1String("  ")) && !line.startsWith(QLatin1String("   ")) && line.endsWith(QLatin1Char(':'))) {
                QVERIFY2(!sections.contains(line), qPrintable(line));
                sections.append(line);
            }
        }
        QCOMPARE(sections.size(), 1 + first.sections.size() + second.sections.size());
        for (const ModuleToc& module : {first, second}) {
            for (const TocSection& section : module.sections) {
                for (const auto& [path, title] : section.pages) {
                    QVERIFY2(toc.contains(QStringLiteral("      %1: ").arg(path)), qPrintable(path));
                }
            }
        }
    }

    void indexLinksEveryModulePageWithItsDescription()
    {
        const ModuleToc toc = converter().moduleToc(allPages());
        const QString index = indexMarkdown({toc});
        QVERIFY(index.startsWith(QLatin1String("---\ntitle: ")));
        for (const TocSection& section : toc.sections) {
            QVERIFY2(index.contains(QStringLiteral("](%1.md): %2").arg(section.page, section.description)), qPrintable(index));
        }
        QVERIFY(index.contains(QLatin1String("](/docs)")));
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
