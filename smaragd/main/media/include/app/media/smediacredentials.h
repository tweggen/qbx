// app/media/smediacredentials.h -- the credential SEAM (proposal 38 §B.8/§C
// GATE 5b).
//
// `main/media` may never see `SSecretStore` (main/shell, §B.8): a provider
// that knew how a password is encrypted would be doing the composition
// root's job, and `SSecretStore` itself must stay reachable from a plain
// ctest with no app_ui linked at all. So this header declares only the
// SEAM -- "give me the Authorization header value for this source id" -- and
// `main/shell` (SMediaAccountManager, GATE 5b) is the one and only
// implementation, installed once at startup. `SWebDavMediaSource` itself
// never calls this: its Authorization header is computed by the CALLER and
// handed to its constructor (§B.8a), so a future Bearer token is a caller
// change, not a client change. This seam exists for the pieces that need a
// header for a source they did not just construct -- gate 5c's reconnect /
// re-auth flow, and anything later that only knows a source's id.
//
// Header-only and DELIBERATELY so: proposal 38 §C GATE 5 restricts gate 5b to
// "adding the credentials header" under main/media/src -- not a new .cpp --
// so the installable-singleton plumbing lives inline here instead of growing
// a translation unit in a module that must stay free of SSecretStore.

#ifndef SMEDIACREDENTIALS_H
#define SMEDIACREDENTIALS_H

#include <QString>

namespace smedia {

// "Give me the Authorization header VALUE for this source id" -- e.g.
// "Basic <base64(user:apppassword)>" today, "Bearer <token>" once Nextcloud
// Login Flow v2 lands (§B.8a). Returns an empty string when no credential is
// available (no such source, an un-remembered session-only password that
// this process instance does not hold, or an `undecryptable` secret) --
// never a placeholder that could be sent as one.
class CredentialProvider
{
public:
    virtual ~CredentialProvider() = default;
    virtual QString authorizationHeaderFor( const QString &sourceId ) const = 0;
};

namespace detail {
inline CredentialProvider *&providerSlot()
{
    static CredentialProvider *p = nullptr;
    return p;
}
} // namespace detail

// The provider installed by the composition root (main/shell), or nullptr
// before startup / after teardown. Never owned here.
inline CredentialProvider *credentialProvider()
{
    return detail::providerSlot();
}

// Installs (or, with nullptr, clears) the process-wide provider. Called
// exactly once at startup and once at teardown by SMediaAccountManager --
// see main/shell/CONTRACT.md.
inline void installCredentialProvider( CredentialProvider *provider )
{
    detail::providerSlot() = provider;
}

} // namespace smedia

#endif // SMEDIACREDENTIALS_H
