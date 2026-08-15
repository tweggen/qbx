#ifndef STESTFILEPATH_H
#define STESTFILEPATH_H

#include <QString>
#include <QStringList>

class SProject;

/**
 * Resolve a filename named by an assert-audio-* verb to a path on disk.
 *
 * The verbs were written to read RENDER OUTPUT, so a bare name has always meant
 * "<testOutputDir>/<name>" and that stays the first and normal answer. But a
 * committed FIXTURE (tests/test_channels4.wav) is not render output and never
 * appears in the output directory, so an assertion aimed at one had no spelling
 * at all — which is why proposal 36 M0 needs this: the multichannel gates assert
 * against a fixture whose channels are known, before any code exists that could
 * produce such a file.
 *
 * Candidates, in order:
 *   1. the path itself, when absolute;
 *   2. <testOutputDir>/<filename>  — render output, the common case;
 *   3. <project sample base dir>/<filename> — the .qxa's own directory, which
 *      is what `add-sample filePath="../test_sawtooth.wav"` already resolves
 *      against, so a fixture is spelled the same way in both places;
 *   4. <cwd>/<filename> — the documented invocation runs a case from
 *      tests/cases/, and this is where 3 lands anyway when a project has no
 *      base dir.
 *
 * The first candidate that EXISTS wins. When none exists the output-dir
 * spelling is returned unchanged, so a missing render still fails with the
 * message it always failed with rather than with a new one.
 *
 * @param filename    Name or relative path as written in the .qxa
 * @param outputDir   The test output directory (may be empty)
 * @param project     Current project, for its sample base dir (may be null)
 * @param tried       Optional out: every candidate considered, for diagnostics
 */
QString resolveTestFilePath(const QString &filename, const QString &outputDir,
                            const SProject *project, QStringList *tried = nullptr);

#endif // STESTFILEPATH_H
