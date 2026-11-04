// SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "converter.h"

#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
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

    /*!
     * QDoc (6.11.3) sometimes writes part of a page after the page's end tag, e.g. KTextEditor's
     * CodeCompletionModel ends with "</db:article>" followed by its HighlightMethod enum section. That
     * isn't well-formed XML; moving the end tag behind the stray content keeps that content on its page.
     */
    QByteArray withStrayContentInside(const QByteArray& xml, bool* moved)
    {
        static const QByteArray endTag = QByteArrayLiteral("</db:article>");
        const qsizetype end = xml.indexOf(endTag);
        *moved = end >= 0 && !xml.mid(end + endTag.size()).trimmed().isEmpty();
        return *moved ? xml.left(end) + xml.mid(end + endTag.size()) + endTag : xml;
    }

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

    //! Escapes Markdown punctuation in plain text. Bare URLs stay as they are: the docs site turns
    //! them into links, and an escape (e.g. "show\_bug.cgi") would end the link early.
    QString escape(const QString& text)
    {
        static const QRegularExpression special(QStringLiteral(R"(([\\`*_\[\]<>|#]))"));
        static const QRegularExpression url(QStringLiteral(R"(\b(?:https?|ftp)://[^\s<>"'`]+)"));
        QString result;
        qsizetype done = 0;
        auto urls = url.globalMatch(text);
        while (urls.hasNext()) {
            const QRegularExpressionMatch match = urls.next();
            result += text.mid(done, match.capturedStart() - done).replace(special, QStringLiteral(R"(\\1)"));
            result += match.captured();
            done = match.capturedEnd();
        }
        return result + text.mid(done).replace(special, QStringLiteral(R"(\\1)"));
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

    // Tags like "[signal]", "[static]" or "[explicit, since 6.23]" at the start of member headings.
    const QRegularExpression& leadingTags()
    {
        static const QRegularExpression pattern(QStringLiteral(R"(^(\[[^\]]+\]\s*)+)"));
        return pattern;
    }

    //! A member heading's text without its parameter names (QDoc marks them as <emphasis>).
    QString titleWithoutParameterNames(const Element& title)
    {
        QString result;
        for (const Node& node : title.children) {
            if (!node.element) {
                result += node.text;
            } else if (node.element->name != QLatin1String("emphasis")) {
                result += node.element->text();
            }
        }
        static const QRegularExpression whitespace(QStringLiteral(R"(\s+)"));
        return result.replace(whitespace, QStringLiteral(" ")).trimmed();
    }

    //! Splits at commas outside brackets, e.g. "QMap<int, int> a, int b" into two parts.
    QStringList splitTopLevel(const QString& text, QChar separator)
    {
        QStringList parts;
        int depth = 0;
        qsizetype start = 0;
        for (qsizetype i = 0; i < text.size(); ++i) {
            const QChar c = text.at(i);
            if (c == QLatin1Char('(') || c == QLatin1Char('<') || c == QLatin1Char('{') || c == QLatin1Char('[')) {
                ++depth;
            } else if (c == QLatin1Char(')') || c == QLatin1Char('>') || c == QLatin1Char('}') || c == QLatin1Char(']')) {
                --depth;
            } else if (c == separator && depth == 0) {
                parts.append(text.mid(start, i - start));
                start = i + 1;
            }
        }
        parts.append(text.mid(start));
        return parts;
    }

    /*!
     * A member heading as Qt's members lists show it: "name(param types) const : return type", without
     * parameter names, default arguments or the class qualifier, and without "void". Headings that
     * aren't functions (properties are "name : type") stay as they are.
     */
    Member memberEntry(const Element& title, const QString& anchor)
    {
        QString text = titleWithoutParameterNames(title);
        text.remove(leadingTags());
        text = text.trimmed();

        // The function name follows the last "::" before the parameter list. The qualifier is the class
        // name as QDoc writes it ("KConfig::", but only "ItemBool::" for KCoreConfigSkeleton::ItemBool).
        const qsizetype open = text.indexOf(QLatin1Char('('));
        const qsizetype at = open < 0 ? -1 : text.lastIndexOf(QLatin1String("::"), open);
        if (at < 0) {
            const QString name = text.section(QLatin1Char(' '), 0, 0);
            return Member{name, text, anchor};
        }
        qsizetype qualifierStart = at;
        while (qualifierStart > 0) {
            const QChar c = text.at(qualifierStart - 1);
            if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char(':') && c != QLatin1Char('<') && c != QLatin1Char('>')) {
                break;
            }
            --qualifierStart;
        }

        const QString returnType = text.left(qualifierStart).trimmed();
        QString rest = text.mid(at + 2);
        // "operator()" has its own parentheses before the parameter list.
        const qsizetype nameEnd = rest.startsWith(QLatin1String("operator()")) ? rest.indexOf(QLatin1Char('('), 9) : rest.indexOf(QLatin1Char('('));
        const QString name = rest.left(nameEnd).trimmed();

        int depth = 0;
        qsizetype close = -1;
        for (qsizetype i = nameEnd; i < rest.size(); ++i) {
            if (rest.at(i) == QLatin1Char('(')) {
                ++depth;
            } else if (rest.at(i) == QLatin1Char(')') && --depth == 0) {
                close = i;
                break;
            }
        }
        if (close < 0) {
            return Member{name, text, anchor};
        }

        QStringList parameters;
        for (QString parameter : splitTopLevel(rest.mid(nameEnd + 1, close - nameEnd - 1), QLatin1Char(','))) {
            const qsizetype defaultAt = splitTopLevel(parameter, QLatin1Char('=')).first().size();
            parameter = parameter.left(defaultAt).trimmed();
            if (!parameter.isEmpty()) {
                parameters.append(parameter);
            }
        }
        static const QRegularExpression constQualifier(QStringLiteral(R"(\bconst\b)"));
        QString signature = name + QLatin1Char('(') + parameters.join(QStringLiteral(", ")) + QLatin1Char(')');
        if (constQualifier.match(rest.mid(close + 1)).hasMatch()) {
            signature += QStringLiteral(" const");
        }
        if (!returnType.isEmpty() && returnType != QLatin1String("void")) {
            signature += QStringLiteral(" : ") + returnType;
        }
        return Member{name, signature, anchor};
    }

    //! The members a documentation section (e.g. "member-function-documentation") documents, by anchor and
    //! heading: each member section's title, and the anchored bridgeheads of members documented together
    //! with it (several \fn in one comment).
    QList<std::pair<QString, const Element*>> memberHeadings(const Element& documentation)
    {
        QList<std::pair<QString, const Element*>> result;
        for (const Element* member : documentation.childElements(QStringLiteral("section"))) {
            result.append({member->attribute(QStringLiteral("id")), member->child(QStringLiteral("title"))});
            for (const Element* bridgehead : member->childElements(QStringLiteral("bridgehead"))) {
                const QString id = bridgehead->attribute(QStringLiteral("id"));
                if (!id.isEmpty()) {
                    result.append({id, bridgehead});
                }
            }
        }
        return result;
    }

    //! A member variable's entry as Qt's members lists show it, "binaryName : QString" for the title
    //! "QString Options::binaryName".
    Member variableEntry(const Element& title, const QString& anchor)
    {
        QString text = title.text();
        text.remove(leadingTags());
        text = text.trimmed();
        const qsizetype at = text.lastIndexOf(QLatin1String("::"));
        const qsizetype nameStart = at < 0 ? text.lastIndexOf(QLatin1Char(' ')) + 1 : at + 2;
        const QString name = text.mid(nameStart);
        qsizetype qualifierStart = at < 0 ? nameStart : at;
        while (qualifierStart > 0) {
            const QChar c = text.at(qualifierStart - 1);
            if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char(':') && c != QLatin1Char('<') && c != QLatin1Char('>')) {
                break;
            }
            --qualifierStart;
        }
        const QString type = text.left(qualifierStart).trimmed();
        return Member{name, type.isEmpty() ? name : name + QStringLiteral(" : ") + type, anchor};
    }

    //! Adds the base classes from the requirements' "Inherits" row: SonicDE ones by page path, Qt ones
    //! with the URL of Qt's all-members page.
    void addBaseClasses(const Element& requirements, const Options& options, ClassInfo* info)
    {
        static const QRegularExpression crossModule(QStringLiteral(R"(^\.\./([^/]+)/([^#]+?)\.html(#.*)?$)"));

        for (const Element* entry : requirements.childElements(QStringLiteral("varlistentry"))) {
            const Element* term = entry->child(QStringLiteral("term"));
            if (!term || term->text().trimmed() != QLatin1String("Inherits")) {
                continue;
            }
            std::function<void(const Element&)> visit = [&](const Element& element) {
                for (const Element* child : element.childElements()) {
                    if (child->name != QLatin1String("link")) {
                        visit(*child);
                        continue;
                    }
                    const QString href = child->attribute(QStringLiteral("href"));
                    const QRegularExpressionMatch cross = crossModule.match(href);
                    if (cross.hasMatch() && QDir(options.qtDocsDir).exists(cross.captured(1))) {
                        info->qtBases.append({child->text().trimmed(), options.qtDocsUrl + cross.captured(2) + QStringLiteral("-members.html")});
                    } else if (cross.hasMatch()) {
                        info->bases.append(cross.captured(1) + QLatin1Char('/') + cross.captured(2));
                    } else if (href.endsWith(QLatin1String(".xml"))) {
                        info->bases.append(options.module + QLatin1Char('/') + href.chopped(4));
                    }
                }
            };
            visit(*entry);
        }
    }

    class PageWriter {
    public:
        explicit PageWriter(const Converter& converter, QString slug = QString(), PageAnchors anchors = {})
            : m_converter(converter)
            , m_slug(std::move(slug))
            , m_anchors(std::move(anchors))
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
        //! Summary sections for the member documentation sections with ids "<prefix>member-...".
        QStringList summaries(const Element& root, const QString& prefix = QString()) const;
        QString requirements(const Element& list) const;

    private:
        static bool isInline(const QString& name);
        QString inlineElement(const Element& element) const;
        QString cellContent(const Element& cell) const;
        QStringList section(const Element& element, int level) const;
        QString table(const Element& element) const;

        const Converter& m_converter;
        //! The page being written and its anchors (including those that moved to its obsolete page).
        QString m_slug;
        PageAnchors m_anchors;
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
            const QString target = m_converter.resolveLink(m_slug, m_anchors, href);
            return target.isEmpty() ? text : QStringLiteral("[%1](%2)").arg(text, target);
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
            if (text.isEmpty()) {
                return {};
            }
            // With an id it is another member documented together with the section's (several \fn):
            // a heading like the section's own, so links to it land.
            const QString id = element.attribute(QStringLiteral("id"));
            if (!id.isEmpty()) {
                return {QStringLiteral("%1 %2 {#%3}").arg(QString(std::clamp(level - 1, 1, 6), QLatin1Char('#')), text, id)};
            }
            return {QStringLiteral("**") + text + QStringLiteral("**")};
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
        if (id != QLatin1String("classes")) {
            return QStringList{heading} + blocks(element, level + 1);
        }

        // QDoc lists structs and unions by their bare name, without a link: link them like its classes.
        QStringList result{heading};
        for (const Element* child : element.childElements()) {
            const Element* entryTitle = child->name == QLatin1String("section") ? child->child(QStringLiteral("title")) : nullptr;
            const auto aggregate = entryTitle && entryTitle->childElements().isEmpty() ? m_converter.aggregatePage(entryTitle->text().trimmed()) : std::nullopt;
            if (!aggregate) {
                result += block(*child, level + 1);
                continue;
            }
            const auto& [page, entry] = *aggregate;
            result.append(QStringLiteral("%1 %2 [%3](%4.md)")
                    .arg(QString(std::min(level + 1, 6), QLatin1Char('#')), entry.typeWord.toLower(), escape(entryTitle->text().trimmed()), page));
            result += blocks(*child, level + 2);
        }
        return result;
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

    QStringList PageWriter::summaries(const Element& root, const QString& prefix) const
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

        static const QRegularExpression tagPattern(QStringLiteral(R"(\[([^\]]+)\])"));
        static const QRegularExpression sinceTag(QStringLiteral(R"(\bsince ([0-9][0-9.]*))"));
        static const QRegularExpression deprecatedTag(QStringLiteral(R"(\bdeprecated in ([0-9][0-9.]*))"));
        static const QRegularExpression qualifier(QStringLiteral(R"(\b[A-Za-z_]\w*::(?=[~A-Za-z_]\w*\s*\())"));

        std::function<void(const Element&)> visit = [&](const Element& element) {
            for (const Element* sectionElement : element.childElements(QStringLiteral("section"))) {
                const QString id = sectionElement->attribute(QStringLiteral("id"));
                const bool isTypes = id == prefix + QStringLiteral("member-type-documentation");
                const bool isProperties = id == prefix + QStringLiteral("property-documentation");
                const bool isFunctions = id == prefix + QStringLiteral("member-function-documentation");

                if (isTypes || isProperties || isFunctions) {
                    for (const auto& [memberId, titleElement] : memberHeadings(*sectionElement)) {
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
                        QString deprecated;
                        const QRegularExpressionMatch leading = leadingTags().match(title);
                        auto tags = tagPattern.globalMatch(leading.captured(0));
                        while (tags.hasNext()) {
                            const QString tag = tags.next().captured(1);
                            const QRegularExpressionMatch sinceMatch = sinceTag.match(tag);
                            if (sinceMatch.hasMatch()) {
                                since = sinceMatch.captured(1);
                            }
                            const QRegularExpressionMatch deprecatedMatch = deprecatedTag.match(tag);
                            if (deprecatedMatch.hasMatch()) {
                                deprecated = deprecatedMatch.captured(1);
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
                        // Summary entries drop the tags and the class qualifier, and show "(since X)"
                        // and "(deprecated in X)", as in Qt's summaries.
                        QString bare = title;
                        bare.remove(leadingTags()).remove(qualifier);
                        QString line = entry.arg(escape(bare));
                        if (!since.isEmpty()) {
                            line += QStringLiteral(" (since %1)").arg(since);
                        }
                        if (!deprecated.isEmpty()) {
                            line += QStringLiteral(" (deprecated in %1)").arg(deprecated);
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

namespace {

    PageAnchors collectAnchors(const Element& root)
    {
        PageAnchors result;
        std::function<void(const Element&, bool)> visit = [&](const Element& element, bool inObsolete) {
            const QString id = element.attribute(QStringLiteral("id"));
            const bool obsolete = inObsolete || (element.name == QLatin1String("section") && id == QLatin1String("obsolete"));
            if (!id.isEmpty()) {
                result.anchors.insert(id);
                if (inObsolete) {
                    result.obsolete.insert(id);
                }
            }
            for (const Element* child : element.childElements()) {
                visit(*child, obsolete);
            }
        };
        visit(root, false);
        return result;
    }

}

std::optional<PageAnchors> Converter::pageAnchors(const QByteArray& xml, QString* error)
{
    bool moved = false;
    const std::unique_ptr<Element> root = parse(withStrayContentInside(xml, &moved), error);
    if (!root) {
        return std::nullopt;
    }
    return collectAnchors(*root);
}

void Converter::setModuleAnchors(QHash<QString, PageAnchors> anchors)
{
    m_moduleAnchors = std::move(anchors);
}

std::optional<QHash<QString, IndexAggregate>> Converter::indexAggregates(const QByteArray& xml, QString* error)
{
    QHash<QString, IndexAggregate> result;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        const QStringView name = reader.name();
        if (name != QLatin1String("struct") && name != QLatin1String("union")) {
            continue;
        }
        const QXmlStreamAttributes attributes = reader.attributes();
        const QString href = attributes.value(QLatin1String("href")).toString();
        // QDoc leaves out fullname when it equals name (global scope).
        const QString fullName = attributes.hasAttribute(QLatin1String("fullname")) ? attributes.value(QLatin1String("fullname")).toString()
                                                                                    : attributes.value(QLatin1String("name")).toString();
        if (href.endsWith(QLatin1String(".html")) && !href.contains(QLatin1Char('#')) && !fullName.isEmpty()) {
            result.insert(href.chopped(5),
                IndexAggregate{fullName, name == QLatin1String("struct") ? QStringLiteral("Struct") : QStringLiteral("Union")});
        }
    }
    if (reader.hasError()) {
        *error = QStringLiteral("%1 (line %2)").arg(reader.errorString()).arg(reader.lineNumber());
        return std::nullopt;
    }
    return result;
}

void Converter::setIndexAggregates(QHash<QString, IndexAggregate> aggregates)
{
    m_aggregates = std::move(aggregates);
}

std::optional<std::pair<QString, IndexAggregate>> Converter::aggregatePage(const QString& title) const
{
    std::optional<std::pair<QString, IndexAggregate>> found;
    for (auto it = m_aggregates.cbegin(); it != m_aggregates.cend(); ++it) {
        if (it->fullName == title || it->fullName.endsWith(QStringLiteral("::") + title)) {
            if (found) {
                sonicDebug() << "The class list entry" << title << "names more than one struct or union";
                return std::nullopt;
            }
            found = std::pair{it.key(), it.value()};
        }
    }
    return found;
}

QString Converter::resolveLink(const QString& fromSlug, const PageAnchors& own, const QString& href) const
{
    static const QRegularExpression otherDocument(QStringLiteral(R"(^([a-z]+://|\.\./))"));
    if (otherDocument.match(href).hasMatch()) {
        return linkTarget(href);
    }

    const qsizetype hash = href.indexOf(QLatin1Char('#'));
    const QString file = hash < 0 ? href : href.left(hash);
    const QString anchor = hash < 0 ? QString() : href.mid(hash + 1);
    if (file.isEmpty()) {
        return linkTarget(href);
    }
    if (!file.endsWith(QLatin1String(".xml"))) {
        // Files QDoc's DocBook output has no page for, e.g. an example's sources ("Tutorial/main.cpp").
        return file.startsWith(QLatin1String("images/")) ? linkTarget(href) : QString();
    }

    const QString slug = file.chopped(4);
    const PageAnchors* target = slug == fromSlug ? &own : nullptr;
    if (!target && !m_moduleAnchors.isEmpty()) {
        const auto it = m_moduleAnchors.constFind(slug);
        if (it == m_moduleAnchors.cend()) {
            sonicDebug() << "Link from" << fromSlug << "to" << slug << "which QDoc wrote no page for; kept as text";
            return QString();
        }
        target = &*it;
    }
    if (!target || anchor.isEmpty()) {
        return linkTarget(href);
    }
    if (target->obsolete.contains(anchor)) {
        return QStringLiteral("%1-obsolete.md#%2").arg(slug, anchor);
    }
    if (!target->anchors.contains(anchor)) {
        // QDoc links to members it has no documentation for (e.g. undocumented properties): link to the
        // page instead, or keep only the text on the page itself.
        sonicDebug() << "Link from" << fromSlug << "to the missing anchor" << slug << "#" << anchor;
        return slug == fromSlug ? QString() : slug + QStringLiteral(".md");
    }
    return linkTarget(href);
}

std::optional<Page> Converter::convert(const QString& slug, const QByteArray& xml, QString* error) const
{
    bool moved = false;
    const std::unique_ptr<Element> root = parse(withStrayContentInside(xml, &moved), error);
    if (moved) {
        sonicWarning() << "Page" << slug << "has content after its end tag (a QDoc defect); it was kept at the end of the page";
    }
    if (!root) {
        return std::nullopt;
    }

    const Element* info = root->child(QStringLiteral("info"));
    const Element* titleElement = info ? info->child(QStringLiteral("title")) : nullptr;
    if (!titleElement) {
        *error = QStringLiteral("the page has no info/title");
        return std::nullopt;
    }
    QString title = titleElement->text().trimmed();
    if (title.isEmpty() && m_aggregates.contains(slug)) {
        // QDoc's DocBook writer titles only classes; Qt's HTML pages call these "<name> Struct" or "<name> Union".
        const IndexAggregate& aggregate = m_aggregates[slug];
        title = aggregate.fullName + QLatin1Char(' ') + aggregate.typeWord;
    }
    static const QRegularExpression classTitle(QStringLiteral(R"( (Class|Struct|Union)$)"));
    const QRegularExpressionMatch classMatch = classTitle.match(title);
    const bool isClass = classMatch.hasMatch();
    const QString className = isClass ? title.left(classMatch.capturedStart()) : QString();
    const bool isNamespace = title.endsWith(QLatin1String(" Namespace"));

    // Deprecated members (QDoc's "obsolete" section) go to their own page, and links to them follow.
    const Element* obsolete = nullptr;
    bool hasDetails = false;
    for (const Element* child : root->childElements(QStringLiteral("section"))) {
        const QString id = child->attribute(QStringLiteral("id"));
        if (id == QLatin1String("obsolete")) {
            obsolete = child;
        } else if (id == QLatin1String("details")) {
            hasDetails = true;
        }
    }
    const PageWriter writer(*this, slug, collectAnchors(*root));

    const Element* abstract = info->child(QStringLiteral("abstract"));
    const QString brief = abstract ? writer.blocks(*abstract, 2).join(QLatin1Char(' ')).trimmed() : QString();

    // Plain-text description for the front matter: link text kept, Markdown removed.
    static const QRegularExpression links(QStringLiteral(R"(\[([^\]]*)\]\([^)]*\))"));
    static const QRegularExpression markup(QStringLiteral(R"([\\*`])"));
    QString description = brief;
    description.replace(links, QStringLiteral("\\1")).remove(markup);

    Page page{slug, title, description, QString(), QString(), std::nullopt};
    if (isClass) {
        page.classInfo = ClassInfo{m_options.module + QLatin1Char('/') + slug, className, {}, {}, {}};
    }

    QStringList body;
    bool requirementsDone = false;
    for (const Element* child : root->childElements()) {
        if (child->name == QLatin1String("info") || child == obsolete) {
            continue;
        }
        if (isClass && !requirementsDone && body.isEmpty() && child->name == QLatin1String("variablelist")) {
            // Requirements block (Header, CMake, Inherits, ...), the member lists, then the rebuilt summaries.
            requirementsDone = true;
            body.append(writer.requirements(*child));
            // A paragraph with line breaks, not a list: Markdown would merge a list into the requirements.
            QStringList memberLinks{QStringLiteral("[List of all members, including inherited members](%1-members.md)").arg(slug)};
            if (obsolete) {
                memberLinks.append(QStringLiteral("[Deprecated members](%1-obsolete.md)").arg(slug));
            }
            body.append(memberLinks.join(QStringLiteral("  \n")));
            body += writer.summaries(*root);
            addBaseClasses(*child, m_options, &*page.classInfo);
            continue;
        }
        if (isNamespace && !requirementsDone && body.isEmpty() && child->name == QLatin1String("variablelist")) {
            requirementsDone = true;
            body.append(writer.requirements(*child));
            continue;
        }
        body += writer.block(*child, 2);
    }

    // Module pages repeat the brief as their first paragraph; class pages don't.
    if (!body.isEmpty() && body.first() == brief) {
        body.removeFirst();
    }

    if (page.classInfo) {
        for (const Element* sectionElement : root->childElements(QStringLiteral("section"))) {
            const QString id = sectionElement->attribute(QStringLiteral("id"));
            const bool variables = id == QLatin1String("member-variable-documentation");
            if (!variables && id != QLatin1String("member-function-documentation") && id != QLatin1String("property-documentation")) {
                continue;
            }
            for (const auto& [anchor, memberTitle] : memberHeadings(*sectionElement)) {
                if (memberTitle) {
                    page.classInfo->members.append(variables ? variableEntry(*memberTitle, anchor) : memberEntry(*memberTitle, anchor));
                }
            }
        }
    }

    QString markdown = QStringLiteral("---\ntitle: %1\ndescription: %2\n---\n\n").arg(yamlString(title), yamlString(description));
    markdown += QStringLiteral("# ") + escape(title) + QStringLiteral("\n\n");
    if (!brief.isEmpty()) {
        // Qt's "More..." jumps from the brief to the Detailed Description.
        markdown += brief + (hasDetails ? QStringLiteral(" [More...](#details)") : QString()) + QStringLiteral("\n\n");
    }
    markdown += joinNonEmpty(body, QStringLiteral("\n\n")) + QLatin1Char('\n');
    page.markdown = markdown;

    if (obsolete) {
        // Like Qt's "<class>-obsolete.html": QDoc's note, the deprecated members' summaries, then their documentation.
        const Element* obsoleteTitle = obsolete->child(QStringLiteral("title"));
        const QString heading = obsoleteTitle ? obsoleteTitle->text().trimmed() : QStringLiteral("Obsolete Members for %1").arg(className);
        QStringList obsoleteBody;
        for (const Element* child : obsolete->childElements()) {
            if (child->name != QLatin1String("section")) {
                obsoleteBody += writer.block(*child, 2);
            }
        }
        obsoleteBody += writer.summaries(*obsolete, QStringLiteral("obsolete-"));
        for (const Element* child : obsolete->childElements(QStringLiteral("section"))) {
            obsoleteBody += writer.block(*child, 2);
        }
        page.obsoleteMarkdown = QStringLiteral("---\ntitle: %1\ndescription: %2\nparent: %3\n---\n\n")
                                    .arg(yamlString(heading),
                                        yamlString(QStringLiteral("Deprecated members of %1.").arg(className.isEmpty() ? title : className)),
                                        yamlString(m_options.module + QLatin1Char('/') + slug));
        page.obsoleteMarkdown += QStringLiteral("# ") + escape(heading) + QStringLiteral("\n\n") + joinNonEmpty(obsoleteBody, QStringLiteral("\n\n")) + QLatin1Char('\n');
    }

    return page;
}

ModuleToc Converter::moduleToc(const QList<Page>& pages) const
{
    const QString& module = m_options.module;
    const Page* modulePage = nullptr;
    const Page* qmlModulePage = nullptr;
    QList<const Page*> classes;
    QList<const Page*> others;
    QList<const Page*> qmlTypes;

    for (const Page& page : pages) {
        if (!modulePage && page.slug.endsWith(QLatin1String("-module"))) {
            modulePage = &page;
        } else if (!qmlModulePage && page.slug.endsWith(QLatin1String("-qmlmodule"))) {
            qmlModulePage = &page;
        } else if (page.slug.startsWith(QLatin1String("qml-"))) {
            qmlTypes.append(&page);
        } else if (page.classInfo) {
            classes.append(&page);
        } else {
            others.append(&page);
        }
    }

    const auto path = [&module](const Page* page) {
        return module + QLatin1Char('/') + page->slug;
    };
    const auto startSection = [&](const Page* sectionPage, const QString& fallbackTitle, bool qml) {
        TocSection section;
        section.title = sectionPage ? sectionPage->title : fallbackTitle;
        section.description = sectionPage ? sectionPage->description : QString();
        section.qml = qml;
        if (sectionPage) {
            section.page = path(sectionPage);
            section.pages.append({section.page, sectionPage->title});
        }
        return section;
    };

    ModuleToc toc;
    toc.module = module;
    if (modulePage || !classes.isEmpty() || !others.isEmpty()) {
        TocSection section = startSection(modulePage, module, false);
        // Classes appear by name, without the " Class" of their page title.
        for (const Page* page : std::as_const(classes)) {
            section.pages.append({path(page), page->classInfo->name});
        }
        for (const Page* page : std::as_const(others)) {
            section.pages.append({path(page), page->title});
        }
        toc.sections.append(section);
    }
    if (qmlModulePage || !qmlTypes.isEmpty()) {
        TocSection section = startSection(qmlModulePage, module + QStringLiteral(" QML Types"), true);
        // QML types appear by name, e.g. "WindowStateSaver" for "WindowStateSaver QML Type".
        for (const Page* page : std::as_const(qmlTypes)) {
            section.pages.append({path(page), QString(page->title).remove(QStringLiteral(" QML Type"))});
        }
        toc.sections.append(section);
    }
    for (const Page& page : pages) {
        if (page.classInfo) {
            toc.classes.append(*page.classInfo);
        }
    }
    return toc;
}

QString ModuleToc::title() const
{
    return sections.isEmpty() ? module : sections.first().title;
}

namespace {

    QJsonArray pairsToJson(const QList<std::pair<QString, QString>>& pairs)
    {
        QJsonArray array;
        for (const auto& [first, second] : pairs) {
            array.append(QJsonArray{first, second});
        }
        return array;
    }

    bool pairsFromJson(const QJsonValue& value, QList<std::pair<QString, QString>>* pairs)
    {
        for (const QJsonValue& item : value.toArray()) {
            const QJsonArray pair = item.toArray();
            if (pair.size() != 2 || !pair.at(0).isString() || !pair.at(1).isString()) {
                return false;
            }
            pairs->append({pair.at(0).toString(), pair.at(1).toString()});
        }
        return true;
    }

}

QByteArray ModuleToc::toJson() const
{
    QJsonArray sectionArray;
    for (const TocSection& section : sections) {
        sectionArray.append(QJsonObject{
            {QStringLiteral("title"), section.title},
            {QStringLiteral("description"), section.description},
            {QStringLiteral("page"), section.page},
            {QStringLiteral("qml"), section.qml},
            {QStringLiteral("pages"), pairsToJson(section.pages)},
        });
    }
    QJsonArray classArray;
    for (const ClassInfo& info : classes) {
        QJsonArray memberArray;
        for (const Member& member : info.members) {
            memberArray.append(QJsonArray{member.name, member.signature, member.anchor});
        }
        classArray.append(QJsonObject{
            {QStringLiteral("page"), info.page},
            {QStringLiteral("name"), info.name},
            {QStringLiteral("bases"), QJsonArray::fromStringList(info.bases)},
            {QStringLiteral("qtBases"), pairsToJson(info.qtBases)},
            {QStringLiteral("members"), memberArray},
        });
    }
    const QJsonObject object{
        {QStringLiteral("module"), module},
        {QStringLiteral("sections"), sectionArray},
        {QStringLiteral("classes"), classArray},
    };
    return QJsonDocument(object).toJson();
}

std::optional<ModuleToc> ModuleToc::fromJson(const QByteArray& json, QString* error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = parseError.errorString();
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    ModuleToc toc;
    toc.module = object.value(QStringLiteral("module")).toString();
    for (const QJsonValue& value : object.value(QStringLiteral("sections")).toArray()) {
        const QJsonObject sectionObject = value.toObject();
        TocSection section;
        section.title = sectionObject.value(QStringLiteral("title")).toString();
        section.description = sectionObject.value(QStringLiteral("description")).toString();
        section.page = sectionObject.value(QStringLiteral("page")).toString();
        section.qml = sectionObject.value(QStringLiteral("qml")).toBool();
        if (!pairsFromJson(sectionObject.value(QStringLiteral("pages")), &section.pages)) {
            *error = QStringLiteral("a page entry isn't [path, title]");
            return std::nullopt;
        }
        if (section.title.isEmpty() || section.pages.isEmpty()) {
            *error = QStringLiteral("a section has no title or pages");
            return std::nullopt;
        }
        toc.sections.append(section);
    }
    for (const QJsonValue& value : object.value(QStringLiteral("classes")).toArray()) {
        const QJsonObject classObject = value.toObject();
        ClassInfo info;
        info.page = classObject.value(QStringLiteral("page")).toString();
        info.name = classObject.value(QStringLiteral("name")).toString();
        for (const QJsonValue& base : classObject.value(QStringLiteral("bases")).toArray()) {
            info.bases.append(base.toString());
        }
        if (!pairsFromJson(classObject.value(QStringLiteral("qtBases")), &info.qtBases)) {
            *error = QStringLiteral("a Qt base isn't [name, url]");
            return std::nullopt;
        }
        for (const QJsonValue& memberValue : classObject.value(QStringLiteral("members")).toArray()) {
            const QJsonArray member = memberValue.toArray();
            if (member.size() != 3) {
                *error = QStringLiteral("a member isn't [name, signature, anchor]");
                return std::nullopt;
            }
            info.members.append(Member{member.at(0).toString(), member.at(1).toString(), member.at(2).toString()});
        }
        if (info.page.isEmpty() || info.name.isEmpty()) {
            *error = QStringLiteral("a class has no page or name");
            return std::nullopt;
        }
        toc.classes.append(info);
    }
    if (toc.module.isEmpty() || toc.sections.isEmpty()) {
        *error = QStringLiteral("module or sections missing");
        return std::nullopt;
    }
    return toc;
}

QString tocYaml(const QList<ModuleToc>& modules)
{
    // Section titles are YAML keys, so they must not repeat: each module section is one contents
    // section, and a title already used gets the module's folder.
    QStringList lines;
    lines << QStringLiteral("rootPage: %1").arg(IndexPage);
    lines << QStringLiteral("sections:");
    lines << QStringLiteral("  \"Overview\":") << QStringLiteral("    pages:");
    lines << QStringLiteral("      %1: \"All Modules\"").arg(IndexPage);
    QStringList usedTitles{QStringLiteral("Overview")};
    for (const ModuleToc& module : modules) {
        for (const TocSection& section : module.sections) {
            QString key = section.title;
            if (usedTitles.contains(key)) {
                key += QStringLiteral(" (%1)").arg(module.module);
            }
            usedTitles.append(key);
            lines << QStringLiteral("  %1:").arg(yamlString(key)) << QStringLiteral("    pages:");
            for (const auto& [path, title] : section.pages) {
                lines << QStringLiteral("      %1: %2").arg(path, yamlString(title));
            }
        }
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString indexMarkdown(const QList<ModuleToc>& modules)
{
    QStringList lines;
    lines << QStringLiteral("---") << QStringLiteral("title: \"All Modules\"")
          << QStringLiteral("description: \"Reference documentation for the SonicDE libraries.\"") << QStringLiteral("---") << QString();
    lines << QStringLiteral("# All Modules") << QString();
    lines << QStringLiteral("Reference documentation for the SonicDE libraries, generated from their source code. "
                            "Guides for using and developing SonicDE are in the [SonicDE Documentation](/docs).")
          << QString();

    // Like Qt's "All Modules" page: C++ modules, then QML modules, each sorted by title.
    for (const bool qml : {false, true}) {
        QList<const TocSection*> sections;
        for (const ModuleToc& module : modules) {
            for (const TocSection& section : module.sections) {
                if (section.qml == qml) {
                    sections.append(&section);
                }
            }
        }
        if (sections.isEmpty()) {
            continue;
        }
        std::stable_sort(sections.begin(), sections.end(), [](const TocSection* a, const TocSection* b) {
            return a->title.compare(b->title, Qt::CaseInsensitive) < 0;
        });
        lines << (qml ? QStringLiteral("## QML Modules {#qml-modules}") : QStringLiteral("## C++ Modules {#modules}")) << QString();
        for (const TocSection* section : std::as_const(sections)) {
            const QString target = (section->page.isEmpty() ? section->pages.first().first : section->page) + QStringLiteral(".md");
            QString line = QStringLiteral("- [%1](%2)").arg(escape(section->title), target);
            if (!section->description.isEmpty()) {
                line += QStringLiteral(": ") + escape(section->description);
            }
            lines << line;
        }
        lines << QString();
    }
    lines.removeLast();
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QList<std::pair<QString, QString>> membersPages(const QList<ModuleToc>& modules)
{
    QHash<QString, const ClassInfo*> byPage;
    for (const ModuleToc& module : modules) {
        for (const ClassInfo& info : module.classes) {
            byPage.insert(info.page, &info);
        }
    }

    QList<std::pair<QString, QString>> result;
    for (const ModuleToc& module : modules) {
        for (const ClassInfo& info : module.classes) {
            // The class and its SonicDE bases, depth first, each once; Qt bases are only linked.
            struct Entry {
                Member member;
                const ClassInfo* owner;
            };
            QList<Entry> entries;
            QList<std::pair<QString, QString>> qtBases;
            QSet<QString> visited;
            std::function<void(const ClassInfo&)> collect = [&](const ClassInfo& current) {
                visited.insert(current.page);
                // Constructors and destructors aren't inherited.
                const QString currentName = current.name.section(QStringLiteral("::"), -1);
                for (const Member& member : current.members) {
                    const bool special = member.name == currentName || member.name == QLatin1Char('~') + currentName;
                    if (&current == &info || !special) {
                        entries.append({member, &current});
                    }
                }
                for (const auto& qtBase : current.qtBases) {
                    if (!qtBases.contains(qtBase)) {
                        qtBases.append(qtBase);
                    }
                }
                for (const QString& base : current.bases) {
                    if (visited.contains(base)) {
                        continue;
                    }
                    if (const ClassInfo* baseInfo = byPage.value(base)) {
                        collect(*baseInfo);
                    } else {
                        sonicInfo() << "The base class" << base << "of" << current.name << "isn't in the reference yet; its members are left out";
                    }
                }
            };
            collect(info);

            // Constructors and the destructor first, then by name, as in Qt's lists.
            const QString shortName = info.name.section(QStringLiteral("::"), -1);
            const auto rank = [&shortName](const Member& member) {
                return member.name == shortName ? 0 : member.name == QLatin1Char('~') + shortName ? 1
                                                                                                  : 2;
            };
            std::stable_sort(entries.begin(), entries.end(), [&rank](const Entry& a, const Entry& b) {
                if (rank(a.member) != rank(b.member)) {
                    return rank(a.member) < rank(b.member);
                }
                const int byName = a.member.name.compare(b.member.name, Qt::CaseInsensitive);
                return byName != 0 ? byName < 0 : a.member.signature < b.member.signature;
            });

            const QString ownModule = info.page.section(QLatin1Char('/'), 0, 0);
            const auto relative = [&ownModule](const QString& page) {
                const QString pageModule = page.section(QLatin1Char('/'), 0, 0);
                const QString file = page.section(QLatin1Char('/'), 1) + QStringLiteral(".md");
                return pageModule == ownModule ? file : QStringLiteral("../%1/%2").arg(pageModule, file);
            };

            const QString heading = QStringLiteral("List of All Members for %1").arg(info.name);
            QStringList lines;
            lines << QStringLiteral("---") << QStringLiteral("title: %1").arg(yamlString(heading))
                  << QStringLiteral("description: %1").arg(yamlString(QStringLiteral("Every member of %1, including inherited members.").arg(info.name)))
                  << QStringLiteral("parent: %1").arg(yamlString(info.page)) << QStringLiteral("---") << QString();
            lines << QStringLiteral("# ") + escape(heading) << QString();
            lines << QStringLiteral("This is the complete list of members for [%1](%2), including inherited members.").arg(escape(info.name), relative(info.page))
                  << QString();
            for (const Entry& entry : std::as_const(entries)) {
                lines << QStringLiteral("- [%1](%2#%3)").arg(escape(entry.member.signature), relative(entry.owner->page), entry.member.anchor);
            }
            if (!qtBases.isEmpty()) {
                QStringList links;
                for (const auto& [name, url] : std::as_const(qtBases)) {
                    links.append(QStringLiteral("[%1](%2)").arg(escape(name), url));
                }
                lines << QString() << QStringLiteral("%1 also inherits the members of %2.").arg(escape(info.name), links.join(QStringLiteral(", ")));
            }
            result.append({info.page + QStringLiteral("-members"), lines.join(QLatin1Char('\n')) + QLatin1Char('\n')});
        }
    }
    return result;
}
}
