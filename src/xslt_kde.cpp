#include "docbookxslt.h"
#ifndef MEINPROC_NO_KARCHIVE
#include <KCompressionDevice>
#else
// Compiled into both KF6DocTools and meinproc6: logs under whichever binary's component, declares none.
#include <Sonic/Logging/Log>
#endif

bool KDocTools::saveToCache(const QString &contents, const QString &filename)
{
#ifndef MEINPROC_NO_KARCHIVE
    KCompressionDevice fd(filename);

    if (!fd.open(QIODevice::WriteOnly)) {
        return false;
    }

    fd.write(contents.toUtf8());
    fd.close();
#else
    sonicWarning() << "This function is dummy because KArchive support has been disabled. "
                      "This mode should be enabled only for specific usage on KDE infrastructure.";
    Q_UNUSED(contents)
    Q_UNUSED(filename)
#endif
    return true;
}
