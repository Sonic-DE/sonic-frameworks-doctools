# KDocTools

Create documentation from DocBook

## Introduction

Provides tools to generate documentation in various format from DocBook files.

## docbook2markdown6

Converts QDoc's DocBook output into Markdown pages for the Winter CMS docs plugin, which
publishes the SonicDE API reference on sonicde.org. Class pages keep the Qt reference layout
(requirements, summary sections, detailed description, member documentation with stable anchors).

1. Generate DocBook with QDoc: add `outputformats = DocBook` to the module's `.qdocconf` and run
   `qdoc` with Qt's documentation index files available (on Gentoo, `dev-qt/qt-docs` with the
   `html` USE flag installs them under `/usr/share/qt6-doc`), so that Qt types are linked.
   Modules that link to each other need each other's `.index` files. Only QDoc's HTML output
   writes one, so first run `qdoc --prepare` with `outputformats = HTML` for every module, then
   `qdoc --generate` with `outputformats = DocBook`, passing the prepare output as `-indexdir`.
2. Convert each module into the same output directory:

   ```sh
   docbook2markdown6 --module kconfigcore <kconfigcore docbook dir> <output dir>
   docbook2markdown6 --module kconfiggui <kconfiggui docbook dir> <output dir>
   ```

   Each run writes `<output dir>/<module>/<page>.md` for every page, copies `images/`, and records
   the module in `<output dir>/<module>/module.json`. A class's deprecated members go to their own
   page, `<page>-obsolete.md`, as on Qt's site. The run then rewrites, from all modules converted
   there, `<output dir>/toc.yaml` (one section for each module's C++ classes and one for its QML
   types), `<output dir>/index.md` (the reference's front page, listing every C++ and QML module)
   and every class's `<page>-members.md` (all members, including those inherited from SonicDE
   classes in other modules, so the order of the runs doesn't matter). Links between modules become relative links, and
   links into Qt's documentation become `https://doc.qt.io/qt-6/` links (`--qt-docs-dir` and
   `--qt-docs-url` change where Qt's docs are found and linked).

## Authors

- Eric Bischoff   (everything not named below ;-)
- Claudiu Costin  (conversion script for Romanian and Russian TeX output)
- Frederik Fouvry (DSSSL style file customisation and SGML setup)
- David Rugge     (DocBook crash course and documentation template)

plus a number of people that made suggestions, improved scripts, sent
bug reports, etc, etc.

