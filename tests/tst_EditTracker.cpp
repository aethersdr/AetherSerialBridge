// EditTracker (INV-13 is closed in WS-10).

#include "core/EditTracker.h"

#include <QTest>

using asb::EditTracker;

class tst_EditTracker : public QObject {
    Q_OBJECT

private slots:
    void startsClean() { QVERIFY(!EditTracker().isDirty()); }

    void anEditMakesItDirty()
    {
        EditTracker t;
        t.markEdited();
        QVERIFY(t.isDirty());
    }

    void aSaveOfTheCurrentRevisionClearsDirty()
    {
        EditTracker t;
        t.markEdited();
        const quint64 saving = t.revision();
        QVERIFY(t.markSaved(saving));
        QVERIFY(!t.isDirty());
    }

    // Kills: markSaved clearing dirty unconditionally (an edit made while Save & apply ran was
    // silently discarded later).
    void anEditDuringTheSaveKeepsItDirty()
    {
        EditTracker t;
        t.markEdited();
        const quint64 saving = t.revision();
        t.markEdited();   // the user typed while the save was running
        QVERIFY(!t.markSaved(saving));
        QVERIFY(t.isDirty());
    }

    // Kills: reset rewinding the revision (e.g. to 0), so a save captured before a reload could match,
    // and clear, edits made after it.
    void aSaveCapturedBeforeAReloadCannotClearLaterEdits()
    {
        EditTracker t;
        t.markEdited();
        const quint64 stale = t.revision();
        t.reset();        // reloaded from disk
        t.markEdited();   // edits the reloaded config
        QVERIFY(!t.markSaved(stale));
        QVERIFY(t.isDirty());
    }

    void resetClearsDirty()
    {
        EditTracker t;
        t.markEdited();
        t.reset();
        QVERIFY(!t.isDirty());
    }

    void revisionOnlyMovesForward()
    {
        EditTracker t;
        QCOMPARE(t.revision(), quint64(0));
        t.markEdited();
        t.markEdited();
        t.reset();
        QCOMPARE(t.revision(), quint64(2));
        QVERIFY(t.markSaved(2));   // a clean tracker reports everything saved
    }
};

QTEST_GUILESS_MAIN(tst_EditTracker)
#include "tst_EditTracker.moc"
