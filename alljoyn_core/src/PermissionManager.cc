/**
 * @file
 * This file defines the Permission DB classes that provide the interface to
 * parse the authorization data
 */

/******************************************************************************
 * Copyright (c) 2014-2015, AllSeen Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for any
 *    purpose with or without fee is hereby granted, provided that the above
 *    copyright notice and this permission notice appear in all copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 ******************************************************************************/

#include <alljoyn/AllJoynStd.h>
#include <alljoyn/Message.h>
#include <alljoyn/PermissionPolicy.h>
#include <qcc/Debug.h>
#include "PermissionManager.h"
#include "BusUtil.h"

#define QCC_MODULE "PERMISSION_MGMT"

using namespace std;
using namespace qcc;

namespace ajn {

struct MessageHolder {
    Message& msg;
    bool send;
    bool propertyRequest;
    bool isSetProperty;
    const char* objPath;
    const char* iName;
    const char* mbrName;

    MessageHolder(Message& msg, bool send) : msg(msg), send(send), propertyRequest(false), isSetProperty(false), iName(NULL), mbrName(NULL)
    {
        objPath = msg->GetObjectPath();
    }
};

struct Right {
    uint8_t authByPolicy;   /* remote peer is authorized by local policy */
    uint8_t authByLocalMembership;  /* local peer is authorized by installed membership certifates */
    uint8_t authByRemoteMembership; /* remote peer is authorized by its membership certificates */

    Right() : authByPolicy(0), authByLocalMembership(0), authByRemoteMembership(0)
    {
    }
};

static bool MatchesPrefix(const char* str, String prefix)
{
    return !WildcardMatch(String(str), prefix);
}

/**
 * Validates whether the request action is explicited denied.
 * @param allowedActions the allowed actions
 * @return true is the requested action is denied; false, otherwise.
 */
static bool IsActionDenied(uint8_t allowedActions)
{
    return (allowedActions & PermissionPolicy::Rule::Member::ACTION_DENIED) == PermissionPolicy::Rule::Member::ACTION_DENIED;
}

/**
 * Validates whether the request action is allowed based on the allow action.
 * @param allowedActions the allowed actions
 * @param requestedAction the request action
 * @return true is the requested action is allowed; false, otherwise.
 */
static bool IsActionAllowed(uint8_t allowedActions, uint8_t requestedAction)
{
    if ((allowedActions & requestedAction) == requestedAction) {
        return true;
    }
    if ((requestedAction == PermissionPolicy::Rule::Member::ACTION_OBSERVE) && ((allowedActions & PermissionPolicy::Rule::Member::ACTION_MODIFY) == PermissionPolicy::Rule::Member::ACTION_MODIFY)) {
        return true; /* lesser right is allowed */
    }
    return false;
}

/**
 * Verify whether the given rule is a match for the given message.
 * If the rule has both object path and interface name, the message must prefix match both.
 * If the rule has object path, the message must prefix match the object path.
 * If the rule has interface name, the message must prefix match the interface name.
 * Find match in member name
 * Verify whether the requested right is allowed by the authorization at the member.
 *      When a member name has an exact match and is explicitly denied access then the rule is not a match.
 *      When a member name has an exact match and is authorized then the rule isa match
 *      When a member name has a prefix match and is authorized then the rule is a match
 *
 */

static bool IsRuleMatched(const PermissionPolicy::Rule& rule, const MessageHolder& msgHolder, uint8_t requiredAuth)
{
    if (rule.GetMembersSize() == 0) {
        return false;
    }
    bool firstPartMatch = false;
    /* Only find match in object path or interface name, but not both */
    if (!rule.GetObjPath().empty()) {
        /* rule has an object path */
        if ((rule.GetObjPath() == msgHolder.objPath) || MatchesPrefix(msgHolder.objPath, rule.GetObjPath())) {
            if (!rule.GetInterfaceName().empty()) {
                /* rule has a specific interface name */
                if (MatchesPrefix(msgHolder.iName, rule.GetInterfaceName())) {
                    firstPartMatch = true;
                }
            } else {
                firstPartMatch = true;
            }
        }
    } else if (!rule.GetInterfaceName().empty()) {
        if ((rule.GetInterfaceName() == msgHolder.iName) || MatchesPrefix(msgHolder.iName, rule.GetInterfaceName())) {
            /* rule has a specific interface name */
            firstPartMatch = true;
        }
    }

    if (!firstPartMatch) {
        return false;
    }

    const PermissionPolicy::Rule::Member* members = rule.GetMembers();
    int8_t* buckets = new int8_t[rule.GetMembersSize()];
    for (size_t cnt = 0; cnt < rule.GetMembersSize(); cnt++) {
        buckets[cnt] = 0;
        if (!members[cnt].GetMemberName().empty()) {
            if (members[cnt].GetMemberName() == msgHolder.mbrName) {
                /* rule has a specific member name match */
                buckets[cnt] = 2;
            } else if (MatchesPrefix(msgHolder.mbrName, members[cnt].GetMemberName())) {
                /* rule has a prefix match member name */
                buckets[cnt] = 1;
            }
            if (buckets[cnt] == 0) {
                continue;
            }
            /* match the member name, now check the action mask */
            if (IsActionDenied(members[cnt].GetActionMask())) {
                buckets[cnt] = -buckets[cnt];
            } else if (!IsActionAllowed(members[cnt].GetActionMask(), requiredAuth)) {
                buckets[cnt] = 0;
            }
        }
    }
    /* now go through the findings */
    for (size_t cnt = 0; cnt < rule.GetMembersSize(); cnt++) {
        if (buckets[cnt] == -2) {
            delete [] buckets;
            return false; /* specifically denied by exact name */
        }
    }
    for (size_t cnt = 0; cnt < rule.GetMembersSize(); cnt++) {
        if (buckets[cnt] == 2) {
            delete [] buckets;
            return true;   /* there is an authorized match with exact name */
        }
    }
    for (size_t cnt = 0; cnt < rule.GetMembersSize(); cnt++) {
        if (buckets[cnt] < 0) {
            delete [] buckets;
            return false;   /* there is a denial based on prefix name match */
        }
    }
    for (size_t cnt = 0; cnt < rule.GetMembersSize(); cnt++) {
        if (buckets[cnt] > 0) {
            delete [] buckets;
            return true;   /* there is an authorized match */
        }
    }
    delete [] buckets;
    return false;
}

static bool IsPolicyTermMatched(const PermissionPolicy::Term& term, const MessageHolder& msgHolder, uint8_t requiredAuth)
{

    const PermissionPolicy::Rule* rules = term.GetRules();
    for (size_t cnt = 0; cnt < term.GetRulesSize(); cnt++) {
        if (IsRuleMatched(rules[cnt], msgHolder, requiredAuth)) {
            return true;
        }
    }
    return false;
}

static bool IsAuthorizedByAnyUserPolicy(const PermissionPolicy* policy, const MessageHolder& msgHolder, uint8_t requiredAuth)
{
    const PermissionPolicy::Term* terms = policy->GetTerms();
    for (size_t cnt = 0; cnt < policy->GetTermsSize(); cnt++) {
        const PermissionPolicy::Peer* peers = terms[cnt].GetPeers();
        bool qualified = false;
        for (size_t idx = 0; idx < terms[cnt].GetPeersSize(); idx++) {
            if (peers[idx].GetType() == PermissionPolicy::Peer::PEER_ANY) {
                qualified = true;
                break;
            }
        }
        if (!qualified) {
            continue;
        }
        if (IsPolicyTermMatched(terms[cnt], msgHolder, requiredAuth)) {
            return true;
        }
    }
    return false;
}

static bool TermHasMatchingGuild(const PermissionPolicy::Term& term, const GUID128& guildGUID)
{
    bool matched = false;
    /* is this peer entry has matching guild GUID */
    const PermissionPolicy::Peer* peers = term.GetPeers();
    for (size_t idx = 0; idx < term.GetPeersSize(); idx++) {
        if ((peers[idx].GetType() == PermissionPolicy::Peer::PEER_GUILD) && peers[idx].GetKeyInfo()) {
            const KeyInfoECC* keyInfo = peers[idx].GetKeyInfo();
            if (keyInfo->GetKeyIdLen() == GUID128::SIZE) {
                GUID128 aGuid(0);
                aGuid.SetBytes(keyInfo->GetKeyId());
                if (aGuid == guildGUID) {
                    matched = true;
                    break;
                }
            }
        }
    }
    return matched;
}

static bool IsAuthorizedByMembership(const GUID128& guildGUID, PermissionPolicy* policy, const MessageHolder& msgHolder, uint8_t requiredAuth)
{
    const PermissionPolicy::Term* terms = policy->GetTerms();
    for (size_t cnt = 0; cnt < policy->GetTermsSize(); cnt++) {
        bool qualified = false;
        if (terms[cnt].GetPeersSize() == 0) {
            qualified = true;  /* there is no peer restriction for this term */
        } else {
            /* look for peer entry with matching guild GUID */
            qualified = TermHasMatchingGuild(terms[cnt], guildGUID);
        }
        if (!qualified) {
            continue;
        }
        if (IsPolicyTermMatched(terms[cnt], msgHolder, requiredAuth)) {
            return true;
        }
    }
    return false;
}

/**
 * Is the given message authorized by any of the remote peer membership certificate auth data?
 */
static bool IsAuthorizedByMembershipCerts(const _PeerState::GuildMap& guildMap, const MessageHolder& msgHolder, uint8_t requiredAuth)
{
    for (_PeerState::GuildMap::const_iterator it = guildMap.begin(); it != guildMap.end(); it++) {
        _PeerState::GuildMetadata* metadata = it->second;
        QCC_DbgTrace(("IsAuthorizedByMembershipCerts with cert %s authData %s\n", metadata->cert.ToString().c_str(), metadata->authData.ToString().c_str()));
        if (IsAuthorizedByMembership(metadata->cert.GetGuild(), &metadata->authData, msgHolder, requiredAuth)) {
            return true;
        }
    }
    return false;
}

/**
 * Is the given message authorized by a guild policy that is common between the peer.
 * The consumer must be both authorized in its membership and in the provider's policy for any guild in common.
 */
static bool IsAuthorizedByGuildsInCommonPolicies(const PermissionPolicy* policy, const MessageHolder& msgHolder, uint8_t policyAuth, PeerState& peerState, uint8_t peerAuth)
{
    for (_PeerState::GuildMap::iterator it = peerState->guildMap.begin(); it != peerState->guildMap.end(); it++) {
        _PeerState::GuildMetadata* metadata = it->second;
        const PermissionPolicy::Term* terms = policy->GetTerms();
        for (size_t cnt = 0; cnt < policy->GetTermsSize(); cnt++) {
            /* look for peer entry with matching guild GUID */
            if (!TermHasMatchingGuild(terms[cnt], metadata->cert.GetGuild())) {
                continue;
            }
            if (IsPolicyTermMatched(terms[cnt], msgHolder, policyAuth)) {
                if (peerAuth == 0) {
                    return true;
                }
                /* validate the peer auth data to make sure it was granted the same thing */
                if (IsAuthorizedByMembership(metadata->cert.GetGuild(), &metadata->authData, msgHolder, peerAuth)) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool IsAuthorizedByPeerPublicKey(const PermissionPolicy* policy, const ECCPublicKey& peerPublicKey, const MessageHolder& msgHolder, uint8_t requiredAuth)
{
    const PermissionPolicy::Term* terms = policy->GetTerms();
    for (size_t cnt = 0; cnt < policy->GetTermsSize(); cnt++) {
        const PermissionPolicy::Peer* peers = terms[cnt].GetPeers();
        bool qualified = false;
        for (size_t idx = 0; idx < terms[cnt].GetPeersSize(); idx++) {
            if ((peers[idx].GetType() == PermissionPolicy::Peer::PEER_GUID) && peers[idx].GetKeyInfo()) {
                if (memcmp(peers[idx].GetKeyInfo()->GetPublicKey(), &peerPublicKey, sizeof(ECCPublicKey)) == 0) {
                    qualified = true;
                    break;
                }
            }
        }
        if (!qualified) {
            continue;
        }
        if (IsPolicyTermMatched(terms[cnt], msgHolder, requiredAuth)) {
            return true;
        }
    }
    return false;
}

static void GenRight(const MessageHolder& msgHolder, Right& right)
{
    if (msgHolder.propertyRequest) {
        if (msgHolder.isSetProperty) {
            /* SetProperty */
            if (msgHolder.send) {
                right.authByLocalMembership = PermissionPolicy::Rule::Member::ACTION_MODIFY;
            } else {
                right.authByPolicy = PermissionPolicy::Rule::Member::ACTION_MODIFY;
            }
        } else {
            /* a GetProperty */
            if (msgHolder.send) {
                right.authByLocalMembership = PermissionPolicy::Rule::Member::ACTION_OBSERVE;
            } else {
                right.authByPolicy = PermissionPolicy::Rule::Member::ACTION_OBSERVE;
            }
        }
        right.authByRemoteMembership = right.authByPolicy;
    } else if (msgHolder.msg->GetType() == MESSAGE_METHOD_CALL) {
        /* a method call */
        if (msgHolder.send) {
            right.authByLocalMembership = PermissionPolicy::Rule::Member::ACTION_MODIFY;
        } else {
            right.authByPolicy = PermissionPolicy::Rule::Member::ACTION_MODIFY;
        }
        right.authByRemoteMembership = right.authByPolicy;
    } else if (msgHolder.msg->GetType() == MESSAGE_SIGNAL) {
        if (msgHolder.send) {
            /* send a signal */
            right.authByLocalMembership = PermissionPolicy::Rule::Member::ACTION_PROVIDE;
        } else {
            /* receive a signal */
            right.authByLocalMembership = PermissionPolicy::Rule::Member::ACTION_OBSERVE;
            right.authByRemoteMembership = PermissionPolicy::Rule::Member::ACTION_PROVIDE;
        }
    }
}

static bool IsAuthorized(const MessageHolder& msgHolder, const PermissionPolicy* policy, const _PeerState::GuildMap& localMembershipMap, PeerState& peerState, PermissionMgmtObj* permissionMgmtObj)
{
    Right right;
    GenRight(msgHolder, right);

    bool authorized = false;

    QCC_DbgPrintf(("IsAuthorized with required permission local %d policy %d remote %d\n", right.authByLocalMembership, right.authByPolicy, right.authByRemoteMembership));
    if (right.authByLocalMembership) {
        /* validate the local peer auth data to make sure it was granted to perform such action */
        if (localMembershipMap.empty()) {
            /* default denied */
            authorized = false;
            QCC_DbgPrintf(("Not authorized because of missing local membership cert"));
        } else {
            authorized = IsAuthorizedByMembershipCerts(localMembershipMap, msgHolder, right.authByLocalMembership);
            QCC_DbgPrintf(("authorized by local membership cert: %d", authorized));
        }
    }

    if (right.authByPolicy) {
        if (policy == NULL) {
            authorized = false;  /* no policy deny all */
            QCC_DbgPrintf(("Not authorized because of missing policy"));
            return false;
        } else {
            /* validate the remote peer auth data to make sure it was granted to perform such action */
            authorized = IsAuthorizedByAnyUserPolicy(policy, msgHolder, right.authByPolicy);
            QCC_DbgPrintf(("authorized by any user policy: %d", authorized));
            if (authorized) {
                right.authByRemoteMembership = 0;  /* mark to skip this check later since it is no longer required */
            }
            if (!authorized) {
                authorized = IsAuthorizedByGuildsInCommonPolicies(policy, msgHolder, right.authByPolicy, peerState, right.authByRemoteMembership);
                right.authByRemoteMembership = 0;  /* mark to skip this check later since it is already done */
                QCC_DbgPrintf(("authorized by guild policy terms in common: %d", authorized));
            }
            if (!authorized) {
                if (!msgHolder.send) {
                    ECCPublicKey peerPublicKey;
                    QStatus status = permissionMgmtObj->GetConnectedPeerPublicKey(peerState->GetGuid(), &peerPublicKey);
                    if (ER_OK != status) {
                        QCC_DbgPrintf(("Failed to retrieve public key from peer session"));
                        return false;
                    }
                    authorized = IsAuthorizedByPeerPublicKey(policy, peerPublicKey, msgHolder, right.authByPolicy);
                    QCC_DbgPrintf(("authorized by peer specific policy terms: %d", authorized));
                    if (authorized) {
                        right.authByRemoteMembership = 0;  /* mark to skip this check later since it is no longer required */
                    }
                }
            }
            if (!authorized) {
                QCC_DbgPrintf(("Not authorized by policy"));
                return false;
            }
        }
    }

    if (right.authByRemoteMembership) {
        if (peerState->guildMap.empty()) {
            authorized = false;
            QCC_DbgPrintf(("Not authorized because of missing peer's membership cert"));
        } else {
            /* validate the remote peer auth data to make sure it was granted to perform such action */
            authorized = IsAuthorizedByMembershipCerts(peerState->guildMap, msgHolder, right.authByRemoteMembership);
            QCC_DbgPrintf(("authorized by peer's membership cert: %d", authorized));
        }
    }
    return authorized;
}

static bool IsStdInterface(const char* iName)
{
    if (strcmp(iName, org::alljoyn::Bus::InterfaceName) == 0) {
        return true;
    }
    if (strcmp(iName, org::alljoyn::Daemon::InterfaceName) == 0) {
        return true;
    }
    if (strcmp(iName, org::alljoyn::Daemon::Debug::InterfaceName) == 0) {
        return true;
    }
    if (strcmp(iName, org::alljoyn::Bus::Peer::Authentication::InterfaceName) == 0) {
        return true;
    }
    if (strcmp(iName, org::alljoyn::Bus::Peer::Session::InterfaceName) == 0) {
        return true;
    }
    if (strcmp(iName, org::allseen::Introspectable::InterfaceName) == 0) {
        return true;
    }
    if (strcmp(iName, org::alljoyn::Bus::Peer::HeaderCompression::InterfaceName) == 0) {
        return true;
    }
    if (strcmp(iName, org::freedesktop::DBus::InterfaceName) == 0) {
        return true;
    }
    if (strcmp(iName, org::freedesktop::DBus::Peer::InterfaceName) == 0) {
        return true;
    }
    if (strcmp(iName, org::freedesktop::DBus::Introspectable::InterfaceName) == 0) {
        return true;
    }
    return false;
}

static bool IsPropertyInterface(const char* iName)
{
    if (strcmp(iName, org::freedesktop::DBus::Properties::InterfaceName) == 0) {
        return true;
    }
    return false;
}

static bool IsPermissionMgmtInterface(const char* iName)
{
    return (strcmp(iName, org::allseen::Security::PermissionMgmt::InterfaceName) == 0);
}

static QStatus ParsePropertiesMessage(MessageHolder& holder)
{
    QStatus status;
    const char* mbrName = holder.msg->GetMemberName();
    const char* propIName;
    const char* propName = "";

    if ((strncmp(mbrName, "Get", 3) == 0) || (strncmp(mbrName, "Set", 3) == 0)) {
        const MsgArg* args;
        size_t numArgs;
        if (holder.send) {
            holder.msg->GetRefArgs(numArgs, args);
        } else {
            holder.msg->GetArgs(numArgs, args);
        }
        if (numArgs < 2) {
            return ER_INVALID_DATA;
        }
        /* only interested in the first two arguments */
        status = args[0].Get("s", &propIName);
        if (ER_OK != status) {
            return status;
        }
        status = args[1].Get("s", &propName);
        if (status != ER_OK) {
            return status;
        }
        holder.propertyRequest = true;
        holder.isSetProperty = (strncmp(mbrName, "Set", 3) == 0);
        QCC_DbgPrintf(("PermissionManager::ParsePropertiesMessage %s %s.%s", mbrName, propIName, propName));
    } else if (strncmp(mbrName, "GetAll", 6) == 0) {
        propName = NULL;
        if (holder.send) {
            const MsgArg* args;
            size_t numArgs;
            holder.msg->GetRefArgs(numArgs, args);
            if (numArgs < 1) {
                return ER_INVALID_DATA;
            }
            status = args[0].Get("s", &propIName);
        } else {
            status = holder.msg->GetArgs("s", &propIName);
        }
        if (status != ER_OK) {
            return status;
        }
        holder.propertyRequest = true;
        QCC_DbgPrintf(("PermissionManager::ParsePropertiesMessage %s %s.%s", mbrName, propIName));
    } else {
        return ER_FAIL;
    }
    holder.iName = propIName;
    holder.mbrName = propName;
    return ER_OK;
}

bool PermissionManager::PeerHasAdminPriv(const GUID128& peerGuid)
{
    ECCPublicKey peerPublicKey;
    QStatus status = permissionMgmtObj->GetConnectedPeerPublicKey(peerGuid, &peerPublicKey);
    if (ER_OK != status) {
        QCC_DbgPrintf(("HsAdminPriv failed to retrieve public key for peer %s", peerGuid.ToString().c_str()));
        return false;
    }
    return permissionMgmtObj->IsTrustAnchor(&peerPublicKey);
}

bool PermissionManager::AuthorizePermissionMgmt(bool send, const GUID128& peerGuid, Message& msg)
{
    if (send) {
        return true;  /* always allow send action */
    }
    bool authorized = false;
    const char* mbrName = msg->GetMemberName();

    if (strncmp(mbrName, "Claim", 5) == 0) {
        /* only allowed when there is no trust anchor */
        return (!permissionMgmtObj->HasTrustAnchors());
    } else if (
        (strncmp(mbrName, "InstallPolicy", 14) == 0) ||
        (strncmp(mbrName, "InstallEncryptedPolicy", 22) == 0) ||
        (strncmp(mbrName, "GetPolicy", 9) == 0) ||
        (strncmp(mbrName, "RemovePolicy", 12) == 0) ||
        (strncmp(mbrName, "InstallMembership", 17) == 0) ||
        (strncmp(mbrName, "InstallMembershipAuthData", 25) == 0) ||
        (strncmp(mbrName, "RemoveMembership", 16) == 0) ||
        (strncmp(mbrName, "InstallIdentity", 15) == 0) ||
        (strncmp(mbrName, "InstallGuildEquivalence", 23) == 0) ||
        (strncmp(mbrName, "RemoveGuildEquivalence", 22) == 0) ||
        (strncmp(mbrName, "Reset", 5) == 0)
        ) {
        /* these actions require admin privilege */
        return PeerHasAdminPriv(peerGuid);
    } else if (
        (strncmp(mbrName, "NotifyConfig", 12) == 0) ||
        (strncmp(mbrName, "GetPublicKey", 12) == 0) ||
        (strncmp(mbrName, "GetIdentity", 11) == 0) ||
        (strncmp(mbrName, "GetManifest", 11) == 0)
        ) {
        return true;
    }
    return authorized;
}

/*
 * the apply order is:
 *  1. applies ANY-USER policy
 *  2. applies all guilds-in-common policies
 *  3. applies peer policies
 */
QStatus PermissionManager::AuthorizeMessage(bool send, Message& msg, PeerState& peerState)
{
    QStatus status = ER_PERMISSION_DENIED;
    bool authorized = false;

    /* only checks for method call and signal */
    if ((msg->GetType() != MESSAGE_METHOD_CALL) &&
        (msg->GetType() != MESSAGE_SIGNAL)) {
        return ER_OK;
    }

    /* skip the AllJoyn Std interfaces */
    if (IsStdInterface(msg->GetInterface())) {
        return ER_OK;
    }
    if (IsPermissionMgmtInterface(msg->GetInterface())) {
        if (!permissionMgmtObj) {
            return ER_PERMISSION_DENIED;
        }
        if (AuthorizePermissionMgmt(send, peerState->GetGuid(), msg)) {
            return ER_OK;
        }
        return ER_PERMISSION_DENIED;
    }
    if (!permissionMgmtObj) {
        return ER_PERMISSION_DENIED;
    }
    /* is the app claimed? If not claimed, no enforcement */
    if (!permissionMgmtObj->HasTrustAnchors()) {
        return ER_OK;
    }

    if (!send && PeerHasAdminPriv(peerState->GetGuid())) {
        QCC_DbgPrintf(("PermissionManager::AuthorizeMessage peer has admin prividege"));
        return ER_OK;  /* admin has full access */
    }
    MessageHolder holder(msg, send);
    if (IsPropertyInterface(msg->GetInterface())) {
        status = ParsePropertiesMessage(holder);
        if (status != ER_OK) {
            return status;
        }
    } else {
        holder.iName = msg->GetInterface();
        holder.mbrName = msg->GetMemberName();
    }

    QCC_DbgPrintf(("PermissionManager::AuthorizeMessage with send: %d msg %s", send, msg->ToString().c_str()));
    authorized = IsAuthorized(holder, GetPolicy(), GetGuildMap(), peerState, permissionMgmtObj);
    if (!authorized) {
        QCC_DbgPrintf(("PermissionManager::AuthorizeMessage IsAuthorized returns ER_PERMISSION_DENIED\n"));
        return ER_PERMISSION_DENIED;
    }
    return ER_OK;
}

} /* namespace ajn */

