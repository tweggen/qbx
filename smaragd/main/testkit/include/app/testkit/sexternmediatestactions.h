#ifndef SEXTERNMEDIATESTACTIONS_H
#define SEXTERNMEDIATESTACTIONS_H

#include "app/actions/saction.h"

#include <QString>

// Headless coverage for the MISSING-SAMPLE PLACEHOLDER and for
// "Collect external media".
//
// Both features are about what a project REFERENCES rather than about what it
// sounds like, so the oracle is the project's own extern-file dictionary. That
// is deliberate and it is the only honest oracle available: a missing sample is
// SILENT, and so is a track that was dropped for having one — an audio
// assertion cannot tell the fix from the bug it replaces.

// assert-extern-files — the shape of SProject::externFiles().
//
//   count    = total extern files the project holds (-1 = do not check)
//   missing  = how many of them are MISSING placeholders    (-1 = skip)
//   external = how many live OUTSIDE the project file's own folder tree
//              (SProject::externalMediaPaths(); -1 = skip). Always 0 for an
//              untitled project — there is nothing to be outside OF.
//
// `missing` is what gates the placeholder: before it, an unreachable sample
// left NO extern file at all (and took its clips with it), so "count=N,
// missing=M" fails in both directions — a build that drops the file fails the
// count, and one that loads it for real fails `missing`.
class SAssertExternFilesAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "assert-extern-files" ); }
    QStringList knownAttributes() const override
    { return { "count", "missing", "external" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int count_    = -1;
    int missing_  = -1;
    int external_ = -1;
};

/**
 * `assert-unused-recordings` — what the Unused recordings dock would list
 * (plan 53 §6.3, P6).
 *
 * It reads `srecfiles::scanUnused`, THE SAME function the dock renders and
 * `cleanup-takes files="1"` would act on. Deliberately not the widget: a
 * scripted run has no dock, and asserting the SCAN is the stronger claim
 * anyway — the dock is a list view over it, and a second implementation of
 * the question is what would let the display and the sweep disagree about
 * which files are orphans.
 *
 *   count    how many entries the scan returns   (-1 = not checked)
 *   contains a substring of some entry's path or reason
 *   absent   a substring NO entry may carry
 *
 * THE DELETION IS NOT REACHABLE FROM HERE, on purpose: `files="1"` moves
 * files to the OS trash, and doing that inside a `-j4` suite is not something
 * a case may do. This verb gates the LIST; the deletion is hand-verified and
 * the PR says so.
 */
class SAssertUnusedRecordingsAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "assert-unused-recordings" ); }
    QStringList knownAttributes() const override
    { return { "count", "contains", "absent" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int     count_ = -1;
    QString contains_;
    QString absent_;
};

// collect-external-media — run the SAME pass the resources dock's button runs
// (smediadrop::collectExternalMedia), with the confirmation dialog and the
// report replaced by attribute assertions.
//
//   expectCopied  = files re-pointed into <projectdir>/media/   (-1 = skip)
//   expectMissing = placeholders skipped (nothing to copy)      (-1 = skip)
//   expectFailed  = copies that did not happen                  (-1 = skip)
class SCollectExternalMediaAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "collect-external-media" ); }
    QStringList knownAttributes() const override
    { return { "expectCopied", "expectMissing", "expectFailed" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int expectCopied_  = -1;
    int expectMissing_ = -1;
    int expectFailed_  = -1;
};

#endif // SEXTERNMEDIATESTACTIONS_H
