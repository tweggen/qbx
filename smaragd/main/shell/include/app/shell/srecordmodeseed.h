#ifndef _SRECORDMODESEED_H_
#define _SRECORDMODESEED_H_

class SProject;

/**
 * Seed a BRAND-NEW project's record mode from the user's INI (plan 53 §5.2).
 *
 * WHY IT LIVES IN shell, AND NOT IN SProjectProps::defaults(). `defaults()` is
 * in `model`, and model may not include `servicesui` where SOpt lives
 * (tools/check_layering.py gives model no app-layer edges at all). So the seed
 * runs where the shell CONSTRUCTS a project that will not be loaded into.
 *
 * And not in `servicesui` either, which is where plan 53 §5.2 put it: SOpt's
 * KEYS are in servicesui but SSettings, which READS them, is in shell, and
 * servicesui may not depend on shell. The helper has to sit beside the reader.
 *
 * CALL IT ONLY FOR A FRESH PROJECT, NEVER AFTER fileOpen(). fileOpen's
 * `new SProject()` is the empty shell the loader fills, and property loading
 * MERGES over whatever is already there
 * (SProject::readPreChildrenAttributes inserts per loaded key), so a seed
 * written there would survive for every file that LACKS the key -- i.e. every
 * project saved before plan 53 would silently read the user's INI value
 * instead of "takes". record_mode_ini_seed.qxa asserts that negative.
 *
 * It writes the property only when the seeded value DIFFERS from
 * SProjectProps::recordModeDefault(), so a user on the default INI gets a
 * project that states nothing and serializes exactly as it did before this
 * feature existed.
 */
namespace srecmodeseed {

/** Map SOpt::MidiRecordMode onto a plan-53 mode and store it on `project`. */
void seedInto( SProject *project );

} // namespace srecmodeseed

#endif // _SRECORDMODESEED_H_
