// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "converter.h"

#include <QDir>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

#include <Sonic/Logging/Log>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace DocBook2Markdown {

namespace {

    const auto DocBookNamespace = QStringLiteral("http://docbook.org/ns/docbook");

    // A small owned tree of the DocBook page: elements keep their local name and attributes (by local
    // name: xml:id is "id", xlink:href is "href"), text stays in document order between child elements.
    struct Element;

    struct Node {
        QString text;
        std::unique_ptr<Element> element;
    };

    struct Element {
        QString name;
        QHash<QString, QString> attributes;
        std::vector<Node> children;

        QString attribute(const QString& key) const
        {
            return attributes.value(key);
        }

        const Element* child(const QString& childName) const
        {
            for (const Node& node : children) {
                if (node.element && node.element->name == childName) {
                    return node.element.get();
                }
            }
            return nullptr;
        }

        QList<const Element*> childElements(const QString& childName = QString()) const
        {
            QList<const Element*> result;
            for (const Node& node : children) {
                if (node.element && (childName.isEmpty() || node.element->name == childName)) {
                    result.append(node.element.get());
                }
            }
            return result;
        }

        //! All text inside, like ElementTree's itertext().
        QString text() const
        {
            QString result;
            for (const Node& node : children) {
                result += node.element ? node.element->text() : node.text;
            }
            return result;
        }
    };

    std::unique_ptr<Element> parse(const QByteArray& xml, QString* error)
    {
        QXmlStreamReader reader(xml);
        std::unique_ptr<Element> root;
        std::vector<Element*> stack;

        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement()) {
                auto element = std::make_unique<Element>();
                element->name = reader.name().toString();
                for (const QXmlStreamAttribute& attribute : reader.attributes()) {
                    element->attributes.insert(attribute.name().toString(), attribute.value().toString());
                }
                Element* raw = element.get();
                if (stack.empty()) {
                    if (reader.namespaceUri() != DocBookNamespace) {
                        *error = QStringLiteral("not a DocBook document (root element namespace \"%1\")").arg(reader.namespaceUri());
                        return nullptr;
                    }
                    root = std::move(element);
                } else {
                    stack.back()->children.push_back(Node{QString(), std::move(element)});
                }
                stack.push_back(raw);
            } else if (reader.isEndElement()) {
                stack.pop_back();
            } else if (reader.isCharacters() && !stack.empty()) {
                stack.back()->children.push_back(Node{reader.text().toString(), nullptr});
            }
        }

        if (reader.hasError()) {
            *error = QStringLiteral("line %1: %2").arg(reader.lineNumber()).arg(reader.errorString());
            return nullptr;
        }
        if (!root) {
            *error = QStringLiteral("the document is empty");
        }
        return root;
    }

    //! Escapes Markdown punctuation in plain text.
    QString escape(const QString& text)
    {
        static const QRegularExpression special(QStringLiteral(R"(([\\`*_\[\]<>|#]))"));
        QString result = text;
        return result.replace(special, QStringLiteral(R"(\\1)"));
    }

    QString yamlString(const QString& text)
    {
        QString escaped = text;
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
        return QLatin1Char('"') + escaped + QLatin1Char('"');
    }

    QString joinNonEmpty(const QStringList& parts, const QString& separator)
    {
        QStringList kept;
        for (const QString& part : parts) {
            if (!part.isEmpty()) {
                kept.append(part);
            }
        }
        return kept.join(separator);
    }

    class PageWriter {
    public:
        explicit PageWriter(const Converter& converter)
            : m_converter(converter)
        {
        }

        QString inlineContent(const Element& element) const
        {
            // Whitespace in running DocBook text is insignificant: newlines and indentation from the XML
            // collapse to single spaces (code blocks take their text verbatim instead).
            static const QRegularExpression whitespace(QStringLiteral(R"(\s+)"));
            QString result;
            for (const Node& node : element.children) {
                QString piece = node.element ? inlineElement(*node.element) : escape(QString(node.text).replace(whitespace, QStringLiteral(" ")));
                // Don't double a space where one piece ends with one and the next starts with one.
                if (result.endsWith(QLatin1Char(' ')) && piece.startsWith(QLatin1Char(' '))) {
                    piece.remove(0, 1);
                }
                result += piece;
            }
            return result;
        }

        QStringList blocks(const Element& element, int level) const
        {
            QStringList result;
            for (const Element* child : element.childElements()) {
                result += block(*child, level);
            }
            return result;
        }

        QStringList block(const Element& element, int level) const;
        QStringList summaries(const Element& root) const;
        QString requirements(const Element& list) const;

    private:
        static bool isInline(const QString& name);
        QString inlineElement(const Element& element) const;
        QString cellContent(const Element& cell) const;
        QStringList section(const Element& element, int level) const;
        QString table(const Element& element) const;

        const Converter& m_converter;
    };

    QString PageWriter::inlineElement(const Element& element) const
    {
        const QString& name = element.name;

        if (name == QLatin1String("link")) {
            const QString href = element.attribute(QStringLiteral("href"));
            QString text = inlineContent(element);
            if (href.isEmpty()) {
                // QDoc writes an empty target for links it couldn't resolve: keep only the text.
                return text;
            }
            if (text.isEmpty()) {
                text = escape(href);
            }
            return QStringLiteral("[%1](%2)").arg(text, m_converter.linkTarget(href));
        }

        if (name == QLatin1String("emphasis")) {
            // Markdown emphasis can't start or end with a space: keep surrounding spaces outside.
            const QString inner = inlineContent(element);
            static const QRegularExpression edges(QStringLiteral(R"(^(\s*)(.*?)(\s*)$)"), QRegularExpression::DotMatchesEverythingOption);
            const QRegularExpressionMatch match = edges.match(inner);
            if (match.captured(2).isEmpty()) {
                return inner;
            }
            const QString mark = element.attribute(QStringLiteral("role")) == QLatin1String("bold") ? QStringLiteral("**") : QStringLiteral("*");
            return match.captured(1) + mark + match.captured(2) + mark + match.captured(3);
        }

        if (name == QLatin1String("simplelist")) {
            QStringList members;
            for (const Element* member : element.childElements(QStringLiteral("member"))) {
                members.append(inlineContent(*member).trimmed());
            }
            return members.join(QStringLiteral(", "));
        }

        static const QStringList codeElements{
            QStringLiteral("code"),
            QStringLiteral("literal"),
            QStringLiteral("function"),
            QStringLiteral("parameter"),
            QStringLiteral("type"),
            QStringLiteral("classname"),
        };
        if (codeElements.contains(name)) {
            return QLatin1Char('`') + element.text().replace(QLatin1Char('`'), QLatin1Char('\'')) + QLatin1Char('`');
        }

        if (name == QLatin1String("superscript")) {
            return QStringLiteral("<sup>") + inlineContent(element) + QStringLiteral("</sup>");
        }
        if (name == QLatin1String("subscript")) {
            return QStringLiteral("<sub>") + inlineContent(element) + QStringLiteral("</sub>");
        }

        return inlineContent(element);
    }

    QStringList PageWriter::block(const Element& element, int level) const
    {
        const QString& name = element.name;

        if (name == QLatin1String("para")) {
            const QString text = inlineContent(element).trimmed();
            return text.isEmpty() ? QStringList() : QStringList{text};
        }

        if (name == QLatin1String("programlisting")) {
            return {QStringLiteral("```%1\n%2\n```").arg(element.attribute(QStringLiteral("language")), element.text().trimmed())};
        }

        if (name == QLatin1String("note") || name == QLatin1String("warning") || name == QLatin1String("important") || name == QLatin1String("tip")) {
            const QString label = name.at(0).toUpper() + name.mid(1);
            const QString body = QStringLiteral("**%1:** %2").arg(label, blocks(element, level).join(QStringLiteral("\n\n")));
            QStringList lines;
            for (const QString& line : body.split(QLatin1Char('\n'))) {
                lines.append(line.isEmpty() ? QStringLiteral(">") : QStringLiteral("> ") + line);
            }
            return {lines.join(QLatin1Char('\n'))};
        }

        if (name == QLatin1String("itemizedlist") || name == QLatin1String("orderedlist")) {
            QStringList items;
            int number = 0;
            for (const Element* item : element.childElements(QStringLiteral("listitem"))) {
                ++number;
                const QString marker = name == QLatin1String("orderedlist") ? QStringLiteral("%1.").arg(number) : QStringLiteral("-");
                QString body = blocks(*item, level).join(QStringLiteral("\n\n"));
                items.append(marker + QLatin1Char(' ') + body.replace(QLatin1Char('\n'), QStringLiteral("\n   ")));
            }
            return {items.join(QLatin1Char('\n'))};
        }

        if (name == QLatin1String("simplelist")) {
            return {inlineElement(element)};
        }

        if (name == QLatin1String("variablelist")) {
            QStringList items;
            for (const Element* entry : element.childElements(QStringLiteral("varlistentry"))) {
                QString term;
                for (const Element* t : entry->childElements(QStringLiteral("term"))) {
                    term += inlineContent(*t);
                }
                QStringList bodies;
                for (const Element* item : entry->childElements(QStringLiteral("listitem"))) {
                    bodies.append(blocks(*item, level).join(QLatin1Char(' ')));
                }
                const QString body = bodies.join(QLatin1Char(' ')).trimmed();
                items.append(QStringLiteral("- ") + term.trimmed() + (body.isEmpty() ? QString() : QStringLiteral(": ") + body));
            }
            return {items.join(QLatin1Char('\n'))};
        }

        if (name == QLatin1String("informaltable") || name == QLatin1String("table")) {
            const QString markdown = table(element);
            return markdown.isEmpty() ? QStringList() : QStringList{markdown};
        }

        if (name == QLatin1String("section")) {
            return section(element, level);
        }

        if (name == QLatin1String("title")) {
            return {};
        }

        if (name == QLatin1String("bridgehead")) {
            // QDoc's second heading line, e.g. "flags KConfig::OpenFlags" under "enum KConfig::OpenFlag".
            const QString text = inlineContent(element).trimmed();
            return text.isEmpty() ? QStringList() : QStringList{QStringLiteral("**") + text + QStringLiteral("**")};
        }

        if (isInline(name)) {
            // An inline element where a block is expected (e.g. a bare <code> in a table cell).
            const QString text = inlineElement(element).trimmed();
            return text.isEmpty() ? QStringList() : QStringList{text};
        }

        if (name != QLatin1String("info")) {
            sonicDebug() << "converting unknown DocBook element" << name << "by its content";
        }
        return blocks(element, level);
    }

    bool PageWriter::isInline(const QString& name)
    {
        static const QStringList inlineElements{
            QStringLiteral("link"),
            QStringLiteral("emphasis"),
            QStringLiteral("code"),
            QStringLiteral("literal"),
            QStringLiteral("function"),
            QStringLiteral("parameter"),
            QStringLiteral("type"),
            QStringLiteral("classname"),
            QStringLiteral("superscript"),
            QStringLiteral("subscript"),
        };
        return inlineElements.contains(name);
    }

    QString PageWriter::cellContent(const Element& cell) const
    {
        // Cells hold either blocks (<para>...) or bare text and inline elements.
        for (const Element* child : cell.childElements()) {
            if (!isInline(child->name)) {
                return blocks(cell, 4).join(QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
            }
        }
        return inlineContent(cell).trimmed();
    }

    QStringList PageWriter::section(const Element& element, int level) const
    {
        const Element* titleElement = element.child(QStringLiteral("title"));
        const QString title = titleElement ? inlineContent(*titleElement).trimmed() : QString();

        if (title == QLatin1String("See Also")) {
            // Qt shows "See also ..." as a closing paragraph, not as a heading.
            return blocks(element, level + 1);
        }

        const QString id = element.attribute(QStringLiteral("id"));
        QString heading = QString(std::min(level, 6), QLatin1Char('#')) + QLatin1Char(' ') + title;
        if (!id.isEmpty()) {
            heading += QStringLiteral(" {#%1}").arg(id);
        }
        return QStringList{heading} + blocks(element, level + 1);
    }

    QString PageWriter::table(const Element& element) const
    {
        QList<QStringList> rows;
        std::function<void(const Element&)> collect = [&](const Element& parent) {
            for (const Element* child : parent.childElements()) {
                if (child->name == QLatin1String("tr")) {
                    QStringList cells;
                    for (const Element* cell : child->childElements()) {
                        if (cell->name == QLatin1String("td") || cell->name == QLatin1String("th")) {
                            const QString text = cellContent(*cell);
                            cells.append(text.isEmpty() ? QStringLiteral(" ") : text);
                        }
                    }
                    rows.append(cells);
                } else {
                    collect(*child);
                }
            }
        };
        collect(element);

        if (rows.isEmpty()) {
            return QString();
        }

        qsizetype width = 0;
        for (const QStringList& row : std::as_const(rows)) {
            width = std::max(width, row.size());
        }
        QStringList lines;
        for (QStringList& row : rows) {
            while (row.size() < width) {
                row.append(QStringLiteral(" "));
            }
        }
        lines.append(QStringLiteral("| ") + rows.first().join(QStringLiteral(" | ")) + QStringLiteral(" |"));
        lines.append(QStringLiteral("|") + QStringLiteral(" --- |").repeated(width));
        for (qsizetype i = 1; i < rows.size(); ++i) {
            lines.append(QStringLiteral("| ") + rows.at(i).join(QStringLiteral(" | ")) + QStringLiteral(" |"));
        }
        return lines.join(QLatin1Char('\n'));
    }

    QStringList PageWriter::summaries(const Element& root) const
    {
        // Summary sections in Qt's order, keyed by the tag QDoc puts in member headings.
        struct Group {
            QString key;
            QString label;
            QStringList entries;
        };
        std::vector<Group> groups{
            {QStringLiteral("property"), QStringLiteral("Properties"), {}},
            {QStringLiteral("public"), QStringLiteral("Public Functions"), {}},
            {QStringLiteral("slot"), QStringLiteral("Public Slots"), {}},
            {QStringLiteral("signal"), QStringLiteral("Signals"), {}},
            {QStringLiteral("static"), QStringLiteral("Static Public Members"), {}},
            {QStringLiteral("protected"), QStringLiteral("Protected Functions"), {}},
        };
        auto group = [&groups](const QString& key) -> QStringList& {
            return std::find_if(groups.begin(), groups.end(), [&key](const Group& g) {
                return g.key == key;
            })->entries;
        };
        QStringList types;

        // Tags like "[signal]", "[static]" or "[explicit, since 6.23]" at the start of member headings.
        static const QRegularExpression tagPattern(QStringLiteral(R"(\[([^\]]+)\])"));
        static const QRegularExpression leadingTags(QStringLiteral(R"(^(\[[^\]]+\]\s*)+)"));
        static const QRegularExpression sinceTag(QStringLiteral(R"(\bsince ([0-9][0-9.]*))"));
        static const QRegularExpression qualifier(QStringLiteral(R"(\b[A-Za-z_]\w*::(?=[~A-Za-z_]\w*\s*\())"));

        std::function<void(const Element&)> visit = [&](const Element& element) {
            for (const Element* sectionElement : element.childElements(QStringLiteral("section"))) {
                const QString id = sectionElement->attribute(QStringLiteral("id"));
                const bool isTypes = id == QLatin1String("member-type-documentation");
                const bool isProperties = id == QLatin1String("property-documentation");
                const bool isFunctions = id == QLatin1String("member-function-documentation");

                if (isTypes || isProperties || isFunctions) {
                    for (const Element* member : sectionElement->childElements(QStringLiteral("section"))) {
                        const QString memberId = member->attribute(QStringLiteral("id"));
                        const Element* titleElement = member->child(QStringLiteral("title"));
                        const QString title = titleElement ? titleElement->text().trimmed() : QString();
                        const QString entry = QStringLiteral("- [%2](#%1)").arg(memberId);

                        if (isTypes) {
                            types.append(entry.arg(escape(title)));
                            continue;
                        }
                        if (isProperties) {
                            group(QStringLiteral("property")).append(entry.arg(escape(title)));
                            continue;
                        }

                        QString key = QStringLiteral("public");
                        QString since;
                        const QRegularExpressionMatch leading = leadingTags.match(title);
                        auto tags = tagPattern.globalMatch(leading.captured(0));
                        while (tags.hasNext()) {
                            const QString tag = tags.next().captured(1);
                            const QRegularExpressionMatch sinceMatch = sinceTag.match(tag);
                            if (sinceMatch.hasMatch()) {
                                since = sinceMatch.captured(1);
                            }
                            if (tag.contains(QLatin1String("signal"))) {
                                key = QStringLiteral("signal");
                            } else if (tag.contains(QLatin1String("slot"))) {
                                key = QStringLiteral("slot");
                            } else if (tag.contains(QLatin1String("static"))) {
                                key = QStringLiteral("static");
                            } else if (tag.contains(QLatin1String("protected"))) {
                                key = QStringLiteral("protected");
                            }
                        }
                        // Summary entries drop the tags and the class qualifier, and show "(since X)",
                        // as in Qt's summaries.
                        QString bare = title;
                        bare.remove(leadingTags).remove(qualifier);
                        QString line = entry.arg(escape(bare));
                        if (!since.isEmpty()) {
                            line += QStringLiteral(" (since %1)").arg(since);
                        }
                        group(key).append(line);
                    }
                }
                visit(*sectionElement);
            }
        };
        visit(root);

        QStringList result;
        if (!types.isEmpty()) {
            result << QStringLiteral("## Public Types") << types.join(QLatin1Char('\n'));
        }
        for (const Group& g : groups) {
            if (!g.entries.isEmpty()) {
                result << QStringLiteral("## ") + g.label << g.entries.join(QLatin1Char('\n'));
            }
        }
        return result;
    }

    QString PageWriter::requirements(const Element& list) const
    {
        static const QStringList codeTerms{QStringLiteral("Header"), QStringLiteral("CMake"), QStringLiteral("qmake")};

        QStringList lines;
        for (const Element* entry : list.childElements(QStringLiteral("varlistentry"))) {
            const Element* termElement = entry->child(QStringLiteral("term"));
            const QString term = termElement ? termElement->text().trimmed() : QString();

            QList<const Element*> paras;
            for (const Element* item : entry->childElements(QStringLiteral("listitem"))) {
                paras += item->childElements(QStringLiteral("para"));
            }

            QStringList parts;
            if (codeTerms.contains(term)) {
                // Build instructions: one code span per line, exactly as QDoc gives them.
                for (const Element* para : std::as_const(paras)) {
                    QString part = para->text().trimmed();
                    if (part.isEmpty()) {
                        continue;
                    }
                    if (term == QLatin1String("Header") && !part.startsWith(QLatin1String("#include"))) {
                        part = QStringLiteral("#include <%1>").arg(part);
                    }
                    parts.append(QLatin1Char('`') + part.replace(QLatin1Char('`'), QLatin1Char('\'')) + QLatin1Char('`'));
                }
                if (!parts.isEmpty()) {
                    lines.append(QStringLiteral("- **%1:** %2").arg(escape(term), parts.join(QStringLiteral("  \n  "))));
                }
            } else {
                for (const Element* para : std::as_const(paras)) {
                    parts.append(inlineContent(*para).trimmed());
                }
                const QString value = joinNonEmpty(parts, QStringLiteral(", "));
                if (!value.isEmpty()) {
                    lines.append(QStringLiteral("- **%1:** %2").arg(escape(term), value));
                }
            }
        }
        return lines.join(QLatin1Char('\n'));
    }

}

Converter::Converter(Options options)
    : m_options(std::move(options))
{
}

QString Converter::linkTarget(const QString& href) const
{
    static const QRegularExpression external(QStringLiteral(R"(^[a-z]+://)"));
    if (external.match(href).hasMatch()) {
        return href;
    }

    // "../<module>/<page>.html#anchor": QDoc's links into other modules' documentation.
    static const QRegularExpression crossModule(QStringLiteral(R"(^\.\./([^/]+)/([^#]+?)\.html(#.*)?$)"));
    const QRegularExpressionMatch cross = crossModule.match(href);
    if (cross.hasMatch()) {
        const QString module = cross.captured(1);
        const QString page = cross.captured(2);
        const QString anchor = cross.captured(3);
        if (QDir(m_options.qtDocsDir).exists(module)) {
            return m_options.qtDocsUrl + page + QStringLiteral(".html") + anchor;
        }
        return QStringLiteral("../%1/%2.md%3").arg(module, page, anchor);
    }

    // "<page>.xml#anchor": a page in this module.
    const qsizetype hash = href.indexOf(QLatin1Char('#'));
    QString page = hash < 0 ? href : href.left(hash);
    const QString anchor = hash < 0 ? QString() : href.mid(hash);
    if (page.endsWith(QLatin1String(".xml"))) {
        page = page.chopped(4) + QStringLiteral(".md");
    }
    const QString target = page + anchor;
    return target.isEmpty() ? QStringLiteral("#") : target;
}

std::optional<Page> Converter::convert(const QString& slug, const QByteArray& xml, QString* error) const
{
    const std::unique_ptr<Element> root = parse(xml, error);
    if (!root) {
        return std::nullopt;
    }

    const PageWriter writer(*this);
    const Element* info = root->child(QStringLiteral("info"));
    const Element* titleElement = info ? info->child(QStringLiteral("title")) : nullptr;
    if (!titleElement) {
        *error = QStringLiteral("the page has no info/title");
        return std::nullopt;
    }
    const QString title = titleElement->text().trimmed();

    const Element* abstract = info->child(QStringLiteral("abstract"));
    const QString brief = abstract ? writer.blocks(*abstract, 2).join(QLatin1Char(' ')).trimmed() : QString();

    // Plain-text description for the front matter: link text kept, Markdown removed.
    static const QRegularExpression links(QStringLiteral(R"(\[([^\]]*)\]\([^)]*\))"));
    static const QRegularExpression markup(QStringLiteral(R"([\\*`])"));
    QString description = brief;
    description.replace(links, QStringLiteral("\\1")).remove(markup);

    const bool isClass = title.endsWith(QLatin1String(" Class"));
    QStringList body;
    bool requirementsDone = false;
    for (const Element* child : root->childElements()) {
        if (child->name == QLatin1String("info")) {
            continue;
        }
        if (isClass && !requirementsDone && body.isEmpty() && child->name == QLatin1String("variablelist")) {
            // Requirements block (Header, CMake, Inherits, ...), then the rebuilt summaries.
            requirementsDone = true;
            body.append(writer.requirements(*child));
            body += writer.summaries(*root);
            continue;
        }
        body += writer.block(*child, 2);
    }

    // Module pages repeat the brief as their first paragraph; class pages don't.
    if (!body.isEmpty() && body.first() == brief) {
        body.removeFirst();
    }

    QString markdown = QStringLiteral("---\ntitle: %1\ndescription: %2\n---\n\n").arg(yamlString(title), yamlString(description));
    markdown += QStringLiteral("# ") + escape(title) + QStringLiteral("\n\n");
    if (!brief.isEmpty()) {
        markdown += brief + QStringLiteral("\n\n");
    }
    markdown += joinNonEmpty(body, QStringLiteral("\n\n")) + QLatin1Char('\n');

    return Page{slug, title, markdown};
}

QString Converter::tocYaml(const QList<Page>& pages) const
{
    const QString& module = m_options.module;
    const Page* modulePage = nullptr;
    QList<const Page*> classes;
    QList<const Page*> others;

    for (const Page& page : pages) {
        if (!modulePage && page.slug.endsWith(QLatin1String("-module"))) {
            modulePage = &page;
        } else if (page.title.endsWith(QLatin1String(" Class"))) {
            classes.append(&page);
        } else {
            others.append(&page);
        }
    }

    QStringList lines;
    if (modulePage) {
        lines << QStringLiteral("rootPage: %1/%2").arg(module, modulePage->slug);
    }
    lines << QStringLiteral("sections:");
    lines << QStringLiteral("  %1:").arg(yamlString(modulePage ? modulePage->title : module));
    lines << QStringLiteral("    pages:");
    if (modulePage) {
        lines << QStringLiteral("      %1/%2: %3").arg(module, modulePage->slug, yamlString(modulePage->title));
    }
    for (const Page* page : std::as_const(classes)) {
        lines << QStringLiteral("      %1/%2: %3").arg(module, page->slug, yamlString(page->title.chopped(6)));
    }
    if (!others.isEmpty()) {
        lines << QStringLiteral("  \"Guides\":") << QStringLiteral("    pages:");
        for (const Page* page : std::as_const(others)) {
            lines << QStringLiteral("      %1/%2: %3").arg(module, page->slug, yamlString(page->title));
        }
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

}
