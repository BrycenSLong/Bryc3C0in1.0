// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <core_io.h>
#include <key_io.h>
#include <policy/policy.h>
#include <rpc/rawtransaction_util.h>
#include <rpc/util.h>
#include <timedata.h>
#include <util/fees.h>
#include <util/rbf.h>
#include <util/translation.h>
#include <util/vector.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>


namespace wallet {
static void ParseRecipients(const UniValue& address_amounts, const UniValue& subtract_fee_outputs, std::vector<CRecipient>& recipients)
{
    std::set<CTxDestination> destinations;
    int i = 0;
    for (const std::string& address: address_amounts.getKeys()) {
        CTxDestination dest = DecodeDestination(address);
        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Brycecoin address: ") + address);
        }

        if (destinations.count(dest)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + address);
        }
        destinations.insert(dest);

        CScript script_pub_key = GetScriptForDestination(dest);
        CAmount amount = AmountFromValue(address_amounts[i++]);

        bool subtract_fee = false;
        for (unsigned int idx = 0; idx < subtract_fee_outputs.size(); idx++) {
            const UniValue& addr = subtract_fee_outputs[idx];
            if (addr.get_str() == address) {
                subtract_fee = true;
            }
        }

        CRecipient recipient = {script_pub_key, amount, subtract_fee};
        recipients.push_back(recipient);
    }
}

static void InterpretFeeEstimationInstructions(const UniValue& conf_target, const UniValue& estimate_mode, const UniValue& fee_rate, UniValue& options)
{
    if (options.exists("conf_target") || options.exists("estimate_mode")) {
        if (!conf_target.isNull() || !estimate_mode.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Pass conf_target and estimate_mode either as arguments or in the options object, but not both");
        }
    } else {
        options.pushKV("conf_target", conf_target);
        options.pushKV("estimate_mode", estimate_mode);
    }
    if (options.exists("fee_rate")) {
        if (!fee_rate.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Pass the fee_rate either as an argument, or in the options object, but not both");
        }
    } else {
        options.pushKV("fee_rate", fee_rate);
    }
    if (!options["conf_target"].isNull() && (options["estimate_mode"].isNull() || (options["estimate_mode"].get_str() == "unset"))) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Specify estimate_mode");
    }
}

static UniValue FinishTransaction(const std::shared_ptr<CWallet> pwallet, const UniValue& options, const CMutableTransaction& rawTx)
{
    // Make a blank psbt
    PartiallySignedTransaction psbtx(rawTx);

    // First fill transaction with our data without signing,
    // so external signers are not asked to sign more than once.
    bool complete;
    pwallet->FillPSBT(psbtx, complete, SIGHASH_DEFAULT, /*sign=*/false, /*bip32derivs=*/true);
    const TransactionError err{pwallet->FillPSBT(psbtx, complete, SIGHASH_DEFAULT, /*sign=*/true, /*bip32derivs=*/false)};
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }

    CMutableTransaction mtx;
    complete = FinalizeAndExtractPSBT(psbtx, mtx);

    UniValue result(UniValue::VOBJ);

    const bool psbt_opt_in{options.exists("psbt") && options["psbt"].get_bool()};
    bool add_to_wallet{options.exists("add_to_wallet") ? options["add_to_wallet"].get_bool() : true};
    if (psbt_opt_in || !complete || !add_to_wallet) {
        // Serialize the PSBT
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << psbtx;
        result.pushKV("psbt", EncodeBase64(ssTx.str()));
    }

    if (complete) {
        std::string hex{EncodeHexTx(CTransaction(mtx))};
        CTransactionRef tx(MakeTransactionRef(std::move(mtx)));
        result.pushKV("txid", tx->GetHash().GetHex());
        if (add_to_wallet && !psbt_opt_in) {
            pwallet->CommitTransaction(tx, {}, /*orderForm=*/{});
        } else {
            result.pushKV("hex", hex);
        }
    }
    result.pushKV("complete", complete);

    return result;
}

static void PreventOutdatedOptions(const UniValue& options)
{
    if (options.exists("feeRate")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use fee_rate (" + CURRENCY_ATOM + "/vB) instead of feeRate");
    }
    if (options.exists("changeAddress")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use change_address instead of changeAddress");
    }
    if (options.exists("changePosition")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use change_position instead of changePosition");
    }
    if (options.exists("includeWatching")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use include_watching instead of includeWatching");
    }
    if (options.exists("lockUnspents")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use lock_unspents instead of lockUnspents");
    }
    if (options.exists("subtractFeeFromOutputs")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Use subtract_fee_from_outputs instead of subtractFeeFromOutputs");
    }
}

UniValue SendMoney(CWallet& wallet, const CCoinControl &coin_control, std::vector<CRecipient> &recipients, mapValue_t map_value, bool verbose)
{
    EnsureWalletIsUnlocked(wallet);

    // This function is only used by sendtoaddress and sendmany.
    // This should always try to sign, if we don't have private keys, don't try to do anything here.
    if (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Private keys are disabled for this wallet");
    }

    // Shuffle recipient list
    std::shuffle(recipients.begin(), recipients.end(), FastRandomContext());

    // Send
    constexpr int RANDOM_CHANGE_POSITION = -1;
    auto res = CreateTransaction(wallet, recipients, RANDOM_CHANGE_POSITION, coin_control, true);
    if (!res) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);
    }
    const CTransactionRef& tx = res->tx;
    wallet.CommitTransaction(tx, std::move(map_value), /*orderForm=*/{});
    if (verbose) {
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("txid", tx->GetHash().GetHex());
        return entry;
    }
    return tx->GetHash().GetHex();
}

RPCHelpMan sendtoaddress()
{
    return RPCHelpMan{"sendtoaddress",
                "\nSend an amount to a given address." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The Brycecoin address to send to."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount in " + CURRENCY_UNIT + " to send. eg 0.1"},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A comment used to store what the transaction is for.\n"
                                         "This is not part of the transaction, just kept in your wallet."},
                    {"comment_to", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A comment to store the name of the person or organization\n"
                                         "to which you're sending the transaction. This is not part of the \n"
                                         "transaction, just kept in your wallet."},
                    {"subtractfeefromamount", RPCArg::Type::BOOL, RPCArg::Default{false}, "The fee will be deducted from the amount being sent.\n"
                                         "The recipient will receive less Brycecoins than you enter in the amount field."},
                    {"avoid_reuse", RPCArg::Type::BOOL, RPCArg::Default{true}, "(only available if avoid_reuse wallet flag is set) Avoid spending from dirty addresses; addresses are considered\n"
                                         "dirty if they have previously been used in a transaction. If true, this also activates avoidpartialspends, grouping outputs by their addresses."},
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "If true, return extra information about the transaction."},
                },
                {
                    RPCResult{"if verbose is not set or set to false",
                        RPCResult::Type::STR_HEX, "txid", "The transaction id."
                    },
                    RPCResult{"if verbose is set to true",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "txid", "The transaction id."},
                            {RPCResult::Type::STR, "fee_reason", "The transaction fee reason."}
                        },
                    },
                },
                RPCExamples{
                    "\nSend 0.1 BRY\n"
                    + HelpExampleCli("sendtoaddress", "\"" + EXAMPLE_ADDRESS[0] + "\" 0.1") +
                    "\nSend 0.1 BRY using positional arguments\n"
                    + HelpExampleCli("sendtoaddress", "\"" + EXAMPLE_ADDRESS[0] + "\" 0.1 \"donation\" \"sean's outpost\" false true") +
                    "\nSend 0.2 BRY using named arguments\n"
                    + HelpExampleCli("-named sendtoaddress", "address=\"" + EXAMPLE_ADDRESS[0] + "\" amount=0.2") +
                    "\nSend 0.5 BRY with a fee rate of 25 " + CURRENCY_ATOM + "/vB using named arguments\n"
                    + HelpExampleCli("-named sendtoaddress", "address=\"" + EXAMPLE_ADDRESS[0] + "\" amount=0.5 fee_rate=25")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK(pwallet->cs_wallet);

    // Wallet comments
    mapValue_t mapValue;
    if (!request.params[2].isNull() && !request.params[2].get_str().empty())
        mapValue["comment"] = request.params[2].get_str();
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["to"] = request.params[3].get_str();

    bool fSubtractFeeFromAmount = false;
    if (!request.params[4].isNull()) {
        fSubtractFeeFromAmount = request.params[4].get_bool();
    }

    CCoinControl coin_control;

    coin_control.m_avoid_address_reuse = GetAvoidReuseFlag(*pwallet, request.params[5]);
    // We also enable partial spend avoidance if reuse avoidance is set.
    coin_control.m_avoid_partial_spends |= coin_control.m_avoid_address_reuse;

    EnsureWalletIsUnlocked(*pwallet);

    UniValue address_amounts(UniValue::VOBJ);
    const std::string address = request.params[0].get_str();
    address_amounts.pushKV(address, request.params[1]);
    UniValue subtractFeeFromAmount(UniValue::VARR);
    if (fSubtractFeeFromAmount) {
        subtractFeeFromAmount.push_back(address);
    }

    std::vector<CRecipient> recipients;
    ParseRecipients(address_amounts, subtractFeeFromAmount, recipients);
    const bool verbose{request.params[6].isNull() ? false : request.params[6].get_bool()};

    return SendMoney(*pwallet, coin_control, recipients, mapValue, verbose);
},
    };
}

RPCHelpMan optimizeutxoset()
{
    return RPCHelpMan{"optimizeutxoset",
                "\nOptimize the UTXO set in order to maximize the PoS yield. This is only valid for continuous minting. The accumulated coinage will be reset!" +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The Brycecoin address to recieve all the new UTXOs. If not provided, new UTOXs will be assigned to the address of the input UTXOs."},
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The " + CURRENCY_UNIT + " amount to set the value of new UTXOs, i.e. make new UTXOs with value of 110. If amount is not provided, hardcoded value will be used."},
                    {"transmit", RPCArg::Type::BOOL, RPCArg::Default{false}, "If true, transmit transaction after generating it."},
                    {"fromAddress", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The Brycecoin address to split coins from. If not provided, all available coins will be used."},
                },
                {
                    RPCResult{"if transmit is not set or set to false",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "tx", /*optional=*/true, "The transaction hex."}
                        },
                    },
                    RPCResult{"if transmit is set to true",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "The transaction id."}
                        },
                    },
                },
                RPCExamples{
                    "\nTrigger UTXO optimization and assign all the new UTXOs to some Brycecoin address with user defined UTXO value\n"
                    + HelpExampleCli("optimizeutxoset", EXAMPLE_ADDRESS[0] + " 110")
               },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK(pwallet->cs_wallet);

    EnsureWalletIsUnlocked(*pwallet);
    CAmount availableCoins = 0;

    mapValue_t mapValue;
    CCoinControl coin_control;
    const std::string address = request.params[0].get_str();
    std::vector<CRecipient> recipients;
    const bool transmit{request.params[2].isNull() ? false : request.params[2].get_bool()};
    int nChangePosRet = -1;
    bilingual_str error;
    CTransactionRef tx;

    if (request.params[3].isNull() == false) {
        std::vector<COutput> vAvailableCoins;
        vAvailableCoins = AvailableCoins(*pwallet, &coin_control).All();
        CTxDestination tmpAddress, fromAddress;
        fromAddress = DecodeDestination(request.params[3].get_str());
        for (const COutput& out : vAvailableCoins) {
            const CScript& scriptPubKey = out.txout.scriptPubKey;
            bool fValidAddress = ExtractDestination(scriptPubKey, tmpAddress);
            if (fValidAddress && (tmpAddress == fromAddress)) {
                coin_control.Select(COutPoint(out.outpoint.hash, out.outpoint.n));
                availableCoins += out.txout.nValue;
            }
        }
        coin_control.m_allow_other_inputs = false;
    } else {
        availableCoins = AvailableCoins(*pwallet).GetTotalAmount();
    }

    if (availableCoins == 0) {
        LogPrintf("no available coins to optimize\n");
        return UniValue::VOBJ;
    }

    LogPrintf("optimizing outputs %d satoshis\n", availableCoins);

    CTxDestination dest = DecodeDestination(address);
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Brycecoin address: ") + address);
    }

    CScript script_pub_key = GetScriptForDestination(dest);
    CAmount amount = AmountFromValue(request.params[1]);
    CAmount remaining = availableCoins;

    CRecipient recipient = {script_pub_key, amount, false};
    CAmount fee = GetMinFee( 2000 + remaining / amount * 40, TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime())); // very approximate
    while (remaining > amount + fee) {
        recipients.push_back(recipient);
        remaining -= amount;
    }

    auto res = CreateTransaction(*pwallet, recipients, nChangePosRet, coin_control, true);
    if (!res) {
        error = util::ErrorString(res);
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, error.original);
    }
    const auto& txr = *res;
    tx = txr.tx;

    UniValue entry(UniValue::VOBJ);
    if (transmit) {
        pwallet->CommitTransaction(tx, std::move(mapValue), {} /* orderForm */);
        entry.pushKV("txid", tx->GetHash().GetHex());
    }
    else {
        entry.pushKV("tx", EncodeHexTx(*tx));
    }
    return entry;
},
    };
}


RPCHelpMan sendmany()
{
    return RPCHelpMan{"sendmany",
        "Send multiple times. Amounts are double-precision floating point numbers." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"dummy", RPCArg::Type::STR, RPCArg::Default{"\"\""}, "Must be set to \"\" for backwards compatibility.",
                     RPCArgOptions{
                         .oneline_description = "\"\"",
                     }},
                    {"amounts", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "The addresses and amounts",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The Brycecoin address is the key, the numeric amount (can be string) in " + CURRENCY_UNIT + " is the value"},
                        },
                    },
                    {"minconf", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Ignored dummy value"},
                    {"comment", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A comment"},
                    {"subtractfeefrom", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "The addresses.\n"
                                       "The fee will be equally deducted from the amount of each selected address.\n"
                                       "Those recipients will receive less Brycecoins than you enter in their corresponding amount field.\n"
                                       "If no addresses are specified here, the sender pays the fee.",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Subtract fee from this address"},
                        },
                    },
                    {"verbose", RPCArg::Type::BOOL, RPCArg::Default{false}, "If true, return extra information about the transaction."},
                },
                {
                    RPCResult{"if verbose is not set or set to false",
                        RPCResult::Type::STR_HEX, "txid", "The transaction id for the send. Only 1 transaction is created regardless of\n"
                "the number of addresses."
                    },
                    RPCResult{"if verbose is set to true",
                        RPCResult::Type::OBJ, "", "",
                        {
                            {RPCResult::Type::STR_HEX, "txid", "The transaction id for the send. Only 1 transaction is created regardless of\n"
                "the number of addresses."},
                            {RPCResult::Type::STR, "fee_reason", "The transaction fee reason."}
                        },
                    },
                },
                RPCExamples{
            "\nSend two amounts to two different addresses:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"" + EXAMPLE_ADDRESS[0] + "\\\":0.01,\\\"" + EXAMPLE_ADDRESS[1] + "\\\":0.02}\"") +
            "\nSend two amounts to two different addresses setting the confirmation and comment:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"" + EXAMPLE_ADDRESS[0] + "\\\":0.01,\\\"" + EXAMPLE_ADDRESS[1] + "\\\":0.02}\" 6 \"testing\"") +
            "\nSend two amounts to two different addresses, subtract fee from amount:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"" + EXAMPLE_ADDRESS[0] + "\\\":0.01,\\\"" + EXAMPLE_ADDRESS[1] + "\\\":0.02}\" 1 \"\" \"[\\\"" + EXAMPLE_ADDRESS[0] + "\\\",\\\"" + EXAMPLE_ADDRESS[1] + "\\\"]\"") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("sendmany", "\"\", {\"" + EXAMPLE_ADDRESS[0] + "\":0.01,\"" + EXAMPLE_ADDRESS[1] + "\":0.02}, 6, \"testing\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK(pwallet->cs_wallet);

    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Dummy value must be set to \"\"");
    }
    UniValue sendTo = request.params[1].get_obj();

    mapValue_t mapValue;
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        mapValue["comment"] = request.params[3].get_str();

    UniValue subtractFeeFromAmount(UniValue::VARR);
    if (!request.params[4].isNull())
        subtractFeeFromAmount = request.params[4].get_array();

    CCoinControl coin_control;

    std::vector<CRecipient> recipients;
    ParseRecipients(sendTo, subtractFeeFromAmount, recipients);
    const bool verbose{request.params[5].isNull() ? false : request.params[5].get_bool()};

    return SendMoney(*pwallet, coin_control, recipients, std::move(mapValue), verbose);
},
    };
}

RPCHelpMan settxfee()
{
    return RPCHelpMan{"settxfee",
                "\nSet the transaction fee rate in " + CURRENCY_UNIT + "/kvB for this wallet. Overrides the global -paytxfee command line parameter.\n"
                "Can be deactivated by passing 0 as the fee. In that case automatic fee selection will be used by default.\n",
                {
                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The transaction fee rate in " + CURRENCY_UNIT + "/kvB"},
                },
                RPCResult{
                    RPCResult::Type::BOOL, "", "Returns true if successful"
                },
                RPCExamples{
                    HelpExampleCli("settxfee", "0.00001")
            + HelpExampleRpc("settxfee", "0.00001")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;

    LOCK(pwallet->cs_wallet);

    return true;
},
    };
}


// Only includes key documentation where the key is snake_case in all RPC methods. MixedCase keys can be added later.
static std::vector<RPCArg> FundTxDoc(bool solving_data = true)
{
    std::vector<RPCArg> args = {
        {"conf_target", RPCArg::Type::NUM, RPCArg::DefaultHint{"wallet -txconfirmtarget"}, "Confirmation target in blocks"},
        {"estimate_mode", RPCArg::Type::STR, RPCArg::Default{"unset"}, "The fee estimate mode, must be one of (case insensitive):\n"
         "\""},
        {
            "replaceable", RPCArg::Type::BOOL, RPCArg::DefaultHint{"wallet default"}, "Marks this transaction as BIP125-replaceable.\n"
            "Allows this transaction to be replaced by a transaction with higher fees"
        },
    };
    if (solving_data) {
        args.push_back({"solving_data", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Keys and scripts needed for producing a final transaction with a dummy signature.\n"
        "Used for fee estimation during coin selection.",
            {
                {
                    "pubkeys", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Public keys involved in this transaction.",
                    {
                        {"pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "A public key"},
                    }
                },
                {
                    "scripts", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Scripts involved in this transaction.",
                    {
                        {"script", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "A script"},
                    }
                },
                {
                    "descriptors", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Descriptors that provide solving data for this transaction.",
                    {
                        {"descriptor", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A descriptor"},
                    }
                },
            }
        });
    }
    return args;
}

void FundTransaction(CWallet& wallet, CMutableTransaction& tx, CAmount& fee_out, int& change_position, const UniValue& options, CCoinControl& coinControl, bool override_min_fee)
{
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    change_position = -1;
    bool lockUnspents = false;
    UniValue subtractFeeFromOutputs;
    std::set<int> setSubtractFeeFromOutputs;

    if (!options.isNull()) {
      if (options.type() == UniValue::VBOOL) {
        // backward compatibility bool only fallback
        coinControl.fAllowWatchOnly = options.get_bool();
      }
      else {
        RPCTypeCheckObj(options,
            {
                {"add_inputs", UniValueType(UniValue::VBOOL)},
                {"include_unsafe", UniValueType(UniValue::VBOOL)},
                {"add_to_wallet", UniValueType(UniValue::VBOOL)},
                {"changeAddress", UniValueType(UniValue::VSTR)},
                {"change_address", UniValueType(UniValue::VSTR)},
                {"changePosition", UniValueType(UniValue::VNUM)},
                {"change_position", UniValueType(UniValue::VNUM)},
                {"change_type", UniValueType(UniValue::VSTR)},
                {"includeWatching", UniValueType(UniValue::VBOOL)},
                {"include_watching", UniValueType(UniValue::VBOOL)},
                {"inputs", UniValueType(UniValue::VARR)},
                {"lockUnspents", UniValueType(UniValue::VBOOL)},
                {"lock_unspents", UniValueType(UniValue::VBOOL)},
                {"locktime", UniValueType(UniValue::VNUM)},
                {"fee_rate", UniValueType()}, // will be checked by AmountFromValue() in SetFeeEstimateMode()
                {"feeRate", UniValueType()}, // will be checked by AmountFromValue() below
                {"psbt", UniValueType(UniValue::VBOOL)},
                {"solving_data", UniValueType(UniValue::VOBJ)},
                {"subtractFeeFromOutputs", UniValueType(UniValue::VARR)},
                {"subtract_fee_from_outputs", UniValueType(UniValue::VARR)},
                {"conf_target", UniValueType(UniValue::VNUM)},
                {"estimate_mode", UniValueType(UniValue::VSTR)},
                {"minconf", UniValueType(UniValue::VNUM)},
                {"maxconf", UniValueType(UniValue::VNUM)},
                {"input_weights", UniValueType(UniValue::VARR)},
            },
            true, true);

        if (options.exists("add_inputs")) {
            coinControl.m_allow_other_inputs = options["add_inputs"].get_bool();
        }

        if (options.exists("changeAddress") || options.exists("change_address")) {
            const std::string change_address_str = (options.exists("change_address") ? options["change_address"] : options["changeAddress"]).get_str();
            CTxDestination dest = DecodeDestination(change_address_str);

            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Change address must be a valid Brycecoin address");
            }

            coinControl.destChange = dest;
        }

        if (options.exists("changePosition") || options.exists("change_position")) {
            change_position = (options.exists("change_position") ? options["change_position"] : options["changePosition"]).getInt<int>();
        }

        if (options.exists("change_type")) {
            if (options.exists("changeAddress") || options.exists("change_address")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both change address and address type options");
            }
            if (std::optional<OutputType> parsed = ParseOutputType(options["change_type"].get_str())) {
                coinControl.m_change_type.emplace(parsed.value());
            } else {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown change type '%s'", options["change_type"].get_str()));
            }
        }

        const UniValue include_watching_option = options.exists("include_watching") ? options["include_watching"] : options["includeWatching"];
        coinControl.fAllowWatchOnly = ParseIncludeWatchonly(include_watching_option, wallet);

        if (options.exists("lockUnspents") || options.exists("lock_unspents")) {
            lockUnspents = (options.exists("lock_unspents") ? options["lock_unspents"] : options["lockUnspents"]).get_bool();
        }

        if (options.exists("include_unsafe")) {
            coinControl.m_include_unsafe_inputs = options["include_unsafe"].get_bool();
        }

        if (options.exists("feeRate")) {
            if (options.exists("fee_rate")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both fee_rate (" + CURRENCY_ATOM + "/vB) and feeRate (" + CURRENCY_UNIT + "/kvB)");
            }
            if (options.exists("conf_target")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both conf_target and feeRate. Please provide either a confirmation target in blocks for automatic fee estimation, or an explicit fee rate.");
            }
            if (options.exists("estimate_mode")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both estimate_mode and feeRate");
            }
        }

        if (options.exists("subtractFeeFromOutputs") || options.exists("subtract_fee_from_outputs") )
            subtractFeeFromOutputs = (options.exists("subtract_fee_from_outputs") ? options["subtract_fee_from_outputs"] : options["subtractFeeFromOutputs"]).get_array();

      }
    } else {
        // if options is null and not a bool
        coinControl.fAllowWatchOnly = ParseIncludeWatchonly(NullUniValue, wallet);
    }

    if (options.exists("solving_data")) {
        const UniValue solving_data = options["solving_data"].get_obj();
        if (solving_data.exists("pubkeys")) {
            for (const UniValue& pk_univ : solving_data["pubkeys"].get_array().getValues()) {
                const std::string& pk_str = pk_univ.get_str();
                if (!IsHex(pk_str)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("'%s' is not hex", pk_str));
                }
                const std::vector<unsigned char> data(ParseHex(pk_str));
                const CPubKey pubkey(data.begin(), data.end());
                if (!pubkey.IsFullyValid()) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("'%s' is not a valid public key", pk_str));
                }
                coinControl.m_external_provider.pubkeys.emplace(pubkey.GetID(), pubkey);
                // Add witness script for pubkeys
                const CScript wit_script = GetScriptForDestination(WitnessV0KeyHash(pubkey));
                coinControl.m_external_provider.scripts.emplace(CScriptID(wit_script), wit_script);
            }
        }

        if (solving_data.exists("scripts")) {
            for (const UniValue& script_univ : solving_data["scripts"].get_array().getValues()) {
                const std::string& script_str = script_univ.get_str();
                if (!IsHex(script_str)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("'%s' is not hex", script_str));
                }
                std::vector<unsigned char> script_data(ParseHex(script_str));
                const CScript script(script_data.begin(), script_data.end());
                coinControl.m_external_provider.scripts.emplace(CScriptID(script), script);
            }
        }

        if (solving_data.exists("descriptors")) {
            for (const UniValue& desc_univ : solving_data["descriptors"].get_array().getValues()) {
                const std::string& desc_str  = desc_univ.get_str();
                FlatSigningProvider desc_out;
                std::string error;
                std::vector<CScript> scripts_temp;
                std::unique_ptr<Descriptor> desc = Parse(desc_str, desc_out, error, true);
                if (!desc) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Unable to parse descriptor '%s': %s", desc_str, error));
                }
                desc->Expand(0, desc_out, scripts_temp, desc_out);
                coinControl.m_external_provider.Merge(std::move(desc_out));
            }
        }
    }

    if (options.exists("input_weights")) {
        for (const UniValue& input : options["input_weights"].get_array().getValues()) {
            uint256 txid = ParseHashO(input, "txid");

            const UniValue& vout_v = find_value(input, "vout");
            if (!vout_v.isNum()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing vout key");
            }
            int vout = vout_v.getInt<int>();
            if (vout < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout cannot be negative");
            }

            const UniValue& weight_v = find_value(input, "weight");
            if (!weight_v.isNum()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing weight key");
            }
            int64_t weight = weight_v.getInt<int64_t>();
            const int64_t min_input_weight = GetTransactionInputWeight(CTxIn());
            CHECK_NONFATAL(min_input_weight == 165);
            if (weight < min_input_weight) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, weight cannot be less than 165 (41 bytes (size of outpoint + sequence + empty scriptSig) * 4 (witness scaling factor)) + 1 (empty witness)");
            }
            if (weight > MAX_STANDARD_TX_WEIGHT) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, weight cannot be greater than the maximum standard tx weight of %d", MAX_STANDARD_TX_WEIGHT));
            }

            coinControl.SetInputWeight(COutPoint(txid, vout), weight);
        }
    }

    if (tx.vout.size() == 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "TX must have at least one output");

    if (change_position != -1 && (change_position < 0 || (unsigned int)change_position > tx.vout.size()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "changePosition out of bounds");

    for (unsigned int idx = 0; idx < subtractFeeFromOutputs.size(); idx++) {
        int pos = subtractFeeFromOutputs[idx].getInt<int>();
        if (setSubtractFeeFromOutputs.count(pos))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated position: %d", pos));
        if (pos < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, negative position: %d", pos));
        if (pos >= int(tx.vout.size()))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, position too large: %d", pos));
        setSubtractFeeFromOutputs.insert(pos);
    }

    bilingual_str error;

    if (!FundTransaction(wallet, tx, fee_out, change_position, error, lockUnspents, setSubtractFeeFromOutputs, coinControl)) {
        throw JSONRPCError(RPC_WALLET_ERROR, error.original);
    }
}

static void SetOptionsInputWeights(const UniValue& inputs, UniValue& options)
{
    if (options.exists("input_weights")) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Input weights should be specified in inputs rather than in options.");
    }
    if (inputs.size() == 0) {
        return;
    }
    UniValue weights(UniValue::VARR);
    for (const UniValue& input : inputs.getValues()) {
        if (input.exists("weight")) {
            weights.push_back(input);
        }
    }
    options.pushKV("input_weights", weights);
}

RPCHelpMan fundrawtransaction()
{
    return RPCHelpMan{"fundrawtransaction",
                "\nIf the transaction has no inputs, they will be automatically selected to meet its out value.\n"
                "It will add at most one change output to the outputs.\n"
                "No existing outputs will be modified unless \"subtractFeeFromOutputs\" is specified.\n"
                "Note that inputs which were signed may need to be resigned after completion since in/outputs have been added.\n"
                "The inputs added will not be signed, use signrawtransactionwithkey\n"
                "or signrawtransactionwithwallet for that.\n"
                "All existing inputs must either have their previous output transaction be in the wallet\n"
                "or be in the UTXO set. Solving data must be provided for non-wallet inputs.\n"
                "Note that all inputs selected must be of standard form and P2SH scripts must be\n"
                "in the wallet using importaddress or addmultisigaddress (to calculate fees).\n"
                "You can see whether this is the case by checking the \"solvable\" field in the listunspent output.\n"
                "Only pay-to-pubkey, multisig, and P2SH versions thereof are currently supported for watch-only\n",
                {
                    {"hexstring", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hex string of the raw transaction"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "for backward compatibility: passing in a true instead of an object will result in {\"includeWatching\":true}",
                        Cat<std::vector<RPCArg>>(
                        {
                            {"add_inputs", RPCArg::Type::BOOL, RPCArg::Default{true}, "For a transaction with existing inputs, automatically include more if they are not enough."},
                            {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions).\n"
                                                          "Warning: the resulting transaction may become invalid if one of the unsafe inputs disappears.\n"
                                                          "If that happens, you will need to fund the transaction with different inputs and republish it."},
                            {"changeAddress", RPCArg::Type::STR, RPCArg::DefaultHint{"pool address"}, "The Brycecoin address to receive the change"},
                            {"changePosition", RPCArg::Type::NUM, RPCArg::DefaultHint{"random"}, "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, RPCArg::DefaultHint{"set by -changetype"}, "The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", \"bech32\", and \"bech32m\"."},
                            {"includeWatching", RPCArg::Type::BOOL, RPCArg::DefaultHint{"true for watch-only wallets, otherwise false"}, "Also select inputs which are watch only.\n"
                                                          "Only solvable inputs can be used. Watch-only destinations are solvable if the public key and/or output script was imported,\n"
                                                          "e.g. with 'importpubkey' or 'importmulti' with the 'pubkeys' or 'desc' field."},
                            {"lockUnspents", RPCArg::Type::BOOL, RPCArg::Default{false}, "Lock selected unspent outputs"},
                            {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_ATOM + "/vB."},
                            {"feeRate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_UNIT + "/kvB."},
                            {"subtractFeeFromOutputs", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "The integers.\n"
                                                          "The fee will be equally deducted from the amount of each specified output.\n"
                                                          "Those recipients will receive less Brycecoins than you enter in their corresponding amount field.\n"
                                                          "If no outputs are specified here, the sender pays the fee.",
                                {
                                    {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                                },
                            },
                            {"input_weights", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Inputs and their corresponding weights",
                                {
                                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                        {
                                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output index"},
                                            {"weight", RPCArg::Type::NUM, RPCArg::Optional::NO, "The maximum weight for this input, "
                                                "including the weight of the outpoint and sequence number. "
                                                "Note that serialized signature sizes are not guaranteed to be consistent, "
                                                "so the maximum DER signatures size of 73 bytes should be used when considering ECDSA signatures."
                                                "Remember to convert serialized sizes to weight units when necessary."},
                                        },
                                    },
                                },
                             },
                        },
                        FundTxDoc()),
                        RPCArgOptions{
                            .skip_type_check = true,
                            .oneline_description = "options",
                        }},
                    {"iswitness", RPCArg::Type::BOOL, RPCArg::DefaultHint{"depends on heuristic tests"}, "Whether the transaction hex is a serialized witness transaction.\n"
                        "If iswitness is not present, heuristic tests will be used in decoding.\n"
                        "If true, only witness deserialization will be tried.\n"
                        "If false, only non-witness deserialization will be tried.\n"
                        "This boolean should reflect whether the transaction has inputs\n"
                        "(e.g. fully valid, or on-chain transactions), if known by the caller."
                    },
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hex", "The resulting raw transaction (hex-encoded string)"},
                        {RPCResult::Type::STR_AMOUNT, "fee", "Fee in " + CURRENCY_UNIT + " the resulting transaction pays"},
                        {RPCResult::Type::NUM, "changepos", "The position of the added change output, or -1"},
                    }
                                },
                                RPCExamples{
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("createrawtransaction", "\"[]\" \"{\\\"myaddress\\\":0.01}\"") +
                            "\nAdd sufficient unsigned inputs to meet the output value\n"
                            + HelpExampleCli("fundrawtransaction", "\"rawtransactionhex\"") +
                            "\nSign the transaction\n"
                            + HelpExampleCli("signrawtransactionwithwallet", "\"fundedtransactionhex\"") +
                            "\nSend the transaction\n"
                            + HelpExampleCli("sendrawtransaction", "\"signedtransactionhex\"")
                                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;

    // parse hex string from parameter
    CMutableTransaction tx;
    bool try_witness = request.params[2].isNull() ? true : request.params[2].get_bool();
    bool try_no_witness = request.params[2].isNull() ? true : !request.params[2].get_bool();
    if (!DecodeHexTx(tx, request.params[0].get_str(), try_no_witness, try_witness)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    CAmount fee;
    int change_position;
    CCoinControl coin_control;
    // Automatically select (additional) coins. Can be overridden by options.add_inputs.
    coin_control.m_allow_other_inputs = true;
    FundTransaction(*pwallet, tx, fee, change_position, request.params[1], coin_control, /*override_min_fee=*/true);

    UniValue result(UniValue::VOBJ);
    result.pushKV("hex", EncodeHexTx(CTransaction(tx)));
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("changepos", change_position);

    return result;
},
    };
}

RPCHelpMan signrawtransactionwithwallet()
{
    return RPCHelpMan{"signrawtransactionwithwallet",
                "\nSign inputs for raw transaction (serialized, hex-encoded).\n"
                "The second optional argument (may be null) is an array of previous transaction outputs that\n"
                "this transaction depends on but may not yet be in the block chain." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"hexstring", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction hex string"},
                    {"prevtxs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "The previous dependent transaction outputs",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"scriptPubKey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "script key"},
                                    {"redeemScript", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2SH) redeem script"},
                                    {"witnessScript", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "(required for P2WSH or P2SH-P2WSH) witness script"},
                                    {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "(required for Segwit inputs) the amount spent"},
                                },
                            },
                        },
                    },
                    {"sighashtype", RPCArg::Type::STR, RPCArg::Default{"DEFAULT for Taproot, ALL otherwise"}, "The signature hash type. Must be one of\n"
            "       \"DEFAULT\"\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\""},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "hex", "The hex-encoded raw transaction with signature(s)"},
                        {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                        {RPCResult::Type::ARR, "errors", /*optional=*/true, "Script verification errors (if there are any)",
                        {
                            {RPCResult::Type::OBJ, "", "",
                            {
                                {RPCResult::Type::STR_HEX, "txid", "The hash of the referenced, previous transaction"},
                                {RPCResult::Type::NUM, "vout", "The index of the output to spent and used as input"},
                                {RPCResult::Type::ARR, "witness", "",
                                {
                                    {RPCResult::Type::STR_HEX, "witness", ""},
                                }},
                                {RPCResult::Type::STR_HEX, "scriptSig", "The hex-encoded signature script"},
                                {RPCResult::Type::NUM, "sequence", "Script sequence number"},
                                {RPCResult::Type::STR, "error", "Verification or signing error related to the input"},
                            }},
                        }},
                    }
                },
                RPCExamples{
                    HelpExampleCli("signrawtransactionwithwallet", "\"myhex\"")
            + HelpExampleRpc("signrawtransactionwithwallet", "\"myhex\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CWallet> pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;

    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed. Make sure the tx has at least one input.");
    }

    // Sign the transaction
    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(*pwallet);

    // Fetch previous transactions (inputs):
    std::map<COutPoint, Coin> coins;
    for (const CTxIn& txin : mtx.vin) {
        coins[txin.prevout]; // Create empty map entry keyed by prevout.
    }
    pwallet->chain().findCoins(coins);

    // Parse the prevtxs array
    ParsePrevouts(request.params[1], nullptr, coins);

    int nHashType = ParseSighashString(request.params[2]);

    // Script verification errors
    std::map<int, bilingual_str> input_errors;

    bool complete = pwallet->SignTransaction(mtx, coins, nHashType, input_errors);
    UniValue result(UniValue::VOBJ);
    SignTransactionResultToJSON(mtx, complete, coins, input_errors, result);
    return result;
},
    };
}

RPCHelpMan send()
{
    return RPCHelpMan{"send",
        "\nEXPERIMENTAL warning: this call may be changed in future releases.\n"
        "\nSend a transaction.\n",
        {
            {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "The outputs (key-value pairs), where none of the keys are duplicated.\n"
                    "That is, each address can only appear once and there can only be one 'data' object.\n"
                    "For convenience, a dictionary, which holds the key-value pairs directly, is also accepted.",
                {
                    {"", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::OMITTED, "",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the Brycecoin address, the value (float or string) is the amount in " + CURRENCY_UNIT + ""},
                        },
                        },
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A key-value pair. The key must be \"data\", the value is hex-encoded data"},
                        },
                    },
                },
                RPCArgOptions{.skip_type_check = true}},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                Cat<std::vector<RPCArg>>(
                {
                    {"add_inputs", RPCArg::Type::BOOL, RPCArg::DefaultHint{"false when \"inputs\" are specified, true otherwise"},"Automatically include coins from the wallet to cover the target amount.\n"},
                    {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions).\n"
                                                          "Warning: the resulting transaction may become invalid if one of the unsafe inputs disappears.\n"
                                                          "If that happens, you will need to fund the transaction with different inputs and republish it."},
                    {"minconf", RPCArg::Type::NUM, RPCArg::Default{0}, "If add_inputs is specified, require inputs with at least this many confirmations."},
                    {"maxconf", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "If add_inputs is specified, require inputs with at most this many confirmations."},
                    {"add_to_wallet", RPCArg::Type::BOOL, RPCArg::Default{true}, "When false, returns a serialized transaction which will not be added to the wallet or broadcast"},
                    {"change_address", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"pool address"}, "The Brycecoin address to receive the change"},
                    {"change_position", RPCArg::Type::NUM, RPCArg::DefaultHint{"random"}, "The index of the change output"},
                    {"change_type", RPCArg::Type::STR, RPCArg::DefaultHint{"set by -changetype"}, "The output type to use. Only valid if change_address is not specified. Options are \"legacy\", \"p2sh-segwit\", \"bech32\" and \"bech32m\"."},
                    {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_ATOM + "/vB."},
                    {"include_watching", RPCArg::Type::BOOL, RPCArg::DefaultHint{"true for watch-only wallets, otherwise false"}, "Also select inputs which are watch only.\n"
                                          "Only solvable inputs can be used. Watch-only destinations are solvable if the public key and/or output script was imported,\n"
                                          "e.g. with 'importpubkey' or 'importmulti' with the 'pubkeys' or 'desc' field."},
                    {"inputs", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Specify inputs instead of adding them automatically. A JSON array of JSON objects",
                        {
                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                            {"sequence", RPCArg::Type::NUM, RPCArg::Optional::NO, "The sequence number"},
                            {"weight", RPCArg::Type::NUM, RPCArg::DefaultHint{"Calculated from wallet and solving data"}, "The maximum weight for this input, "
                                        "including the weight of the outpoint and sequence number. "
                                        "Note that signature sizes are not guaranteed to be consistent, "
                                        "so the maximum DER signatures size of 73 bytes should be used when considering ECDSA signatures."
                                        "Remember to convert serialized sizes to weight units when necessary."},
                        },
                    },
                    {"locktime", RPCArg::Type::NUM, RPCArg::Default{0}, "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"timestamp", RPCArg::Type::NUM, RPCArg::Default{0}, "Transaction timestamp"},
                    {"lock_unspents", RPCArg::Type::BOOL, RPCArg::Default{false}, "Lock selected unspent outputs"},
                    {"psbt", RPCArg::Type::BOOL,  RPCArg::DefaultHint{"automatic"}, "Always return a PSBT, implies add_to_wallet=false."},
                    {"subtract_fee_from_outputs", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Outputs to subtract the fee from, specified as integer indices.\n"
                    "The fee will be equally deducted from the amount of each specified output.\n"
                    "Those recipients will receive less Brycecoins than you enter in their corresponding amount field.\n"
                    "If no outputs are specified here, the sender pays the fee.",
                        {
                            {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                        },
                    },
                },
                FundTxDoc()),
                RPCArgOptions{.oneline_description="options"}},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                    {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "The transaction id for the send. Only 1 transaction is created regardless of the number of addresses."},
                    {RPCResult::Type::STR_HEX, "hex", /*optional=*/true, "If add_to_wallet is false, the hex-encoded raw transaction with signature(s)"},
                    {RPCResult::Type::STR, "psbt", /*optional=*/true, "If more signatures are needed, or if add_to_wallet is false, the base64-encoded (partially) signed transaction"}
                }
        },
        RPCExamples{""
        "\nSend 0.1 BTC with a confirmation target of 6 blocks in economical fee estimate mode\n"
        + HelpExampleCli("send", "'{\"" + EXAMPLE_ADDRESS[0] + "\": 0.1}' 6 economical\n") +
        "Send 0.2 BTC with a fee rate of 1.1 " + CURRENCY_ATOM + "/vB using positional arguments\n"
        + HelpExampleCli("send", "'{\"" + EXAMPLE_ADDRESS[0] + "\": 0.2}' null \"unset\" 1.1\n") +
        "Send 0.2 BTC with a fee rate of 1 " + CURRENCY_ATOM + "/vB using the options argument\n"
        + HelpExampleCli("send", "'{\"" + EXAMPLE_ADDRESS[0] + "\": 0.2}' null \"unset\" null '{\"fee_rate\": 1}'\n") +
        "Send 0.3 BTC with a fee rate of 25 " + CURRENCY_ATOM + "/vB using named arguments\n"
        + HelpExampleCli("-named send", "outputs='{\"" + EXAMPLE_ADDRESS[0] + "\": 0.3}' fee_rate=25\n") +
        "Create a transaction that should confirm the next block, with a specific input, and return result without adding to wallet or broadcasting to the network\n"
        + HelpExampleCli("send", "'{\"" + EXAMPLE_ADDRESS[0] + "\": 0.1}' 1 economical '{\"add_to_wallet\": false, \"inputs\": [{\"txid\":\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\", \"vout\":1}]}'")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;

            UniValue options{request.params[1].isNull() ? UniValue::VOBJ : request.params[1]};
//            InterpretFeeEstimationInstructions(/*conf_target=*/request.params[1], /*estimate_mode=*/request.params[2], /*fee_rate=*/request.params[3], options);
            PreventOutdatedOptions(options);


            CAmount fee;
            int change_position;
            CMutableTransaction rawTx = ConstructTransaction(options["inputs"], request.params[0], options["locktime"], options["timestamp"]);
            CCoinControl coin_control;
            // Automatically select coins, unless at least one is manually selected. Can
            // be overridden by options.add_inputs.
            coin_control.m_allow_other_inputs = rawTx.vin.size() == 0;
            SetOptionsInputWeights(options["inputs"], options);
            FundTransaction(*pwallet, rawTx, fee, change_position, options, coin_control, /*override_min_fee=*/false);

            return FinishTransaction(pwallet, options, rawTx);
        }
    };
}

RPCHelpMan sendall()
{
    return RPCHelpMan{"sendall",
        "EXPERIMENTAL warning: this call may be changed in future releases.\n"
        "\nSpend the value of all (or specific) confirmed UTXOs in the wallet to one or more recipients.\n"
        "Unconfirmed inbound UTXOs and locked UTXOs will not be spent. Sendall will respect the avoid_reuse wallet flag.\n"
        "If your wallet contains many small inputs, either because it received tiny payments or as a result of accumulating change, consider using `send_max` to exclude inputs that are worth less than the fees needed to spend them.\n",
        {
            {"recipients", RPCArg::Type::ARR, RPCArg::Optional::NO, "The sendall destinations. Each address may only appear once.\n"
                "Optionally some recipients can be specified with an amount to perform payments, but at least one address must appear without a specified amount.\n",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "A bitcoin address which receives an equal share of the unspecified amount."},
                    {"", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::OMITTED, "",
                        {
                            {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the bitcoin address, the value (float or string) is the amount in " + CURRENCY_UNIT + ""},
                        },
                    },
                },
            },
            {"conf_target", RPCArg::Type::NUM, RPCArg::DefaultHint{"wallet -txconfirmtarget"}, "Confirmation target in blocks"},
            {"estimate_mode", RPCArg::Type::STR, RPCArg::Default{"unset"}, "The fee estimate mode, must be one of (case insensitive):\n"
             "\""},
            {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_ATOM + "/vB."},
            {
                "options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                Cat<std::vector<RPCArg>>(
                    {
                        {"add_to_wallet", RPCArg::Type::BOOL, RPCArg::Default{true}, "When false, returns the serialized transaction without broadcasting or adding it to the wallet"},
                        {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_ATOM + "/vB."},
                        {"include_watching", RPCArg::Type::BOOL, RPCArg::DefaultHint{"true for watch-only wallets, otherwise false"}, "Also select inputs which are watch-only.\n"
                                              "Only solvable inputs can be used. Watch-only destinations are solvable if the public key and/or output script was imported,\n"
                                              "e.g. with 'importpubkey' or 'importmulti' with the 'pubkeys' or 'desc' field."},
                        {"inputs", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "Use exactly the specified inputs to build the transaction. Specifying inputs is incompatible with the send_max, minconf, and maxconf options.",
                            {
                                {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                    {
                                        {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                        {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                        {"sequence", RPCArg::Type::NUM, RPCArg::DefaultHint{"depends on the value of the 'replaceable' and 'locktime' arguments"}, "The sequence number"},
                                    },
                                },
                            },
                        },
                        {"locktime", RPCArg::Type::NUM, RPCArg::Default{0}, "Raw locktime. Non-0 value also locktime-activates inputs"},
                        {"lock_unspents", RPCArg::Type::BOOL, RPCArg::Default{false}, "Lock selected unspent outputs"},
                        {"psbt", RPCArg::Type::BOOL,  RPCArg::DefaultHint{"automatic"}, "Always return a PSBT, implies add_to_wallet=false."},
                        {"send_max", RPCArg::Type::BOOL, RPCArg::Default{false}, "When true, only use UTXOs that can pay for their own fees to maximize the output amount. When 'false' (default), no UTXO is left behind. send_max is incompatible with providing specific inputs."},
                        {"minconf", RPCArg::Type::NUM, RPCArg::Default{0}, "Require inputs with at least this many confirmations."},
                        {"maxconf", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Require inputs with at most this many confirmations."},
                    },
                    FundTxDoc()
                ),
                RPCArgOptions{.oneline_description="options"}
            },
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                    {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "The transaction id for the send. Only 1 transaction is created regardless of the number of addresses."},
                    {RPCResult::Type::STR_HEX, "hex", /*optional=*/true, "If add_to_wallet is false, the hex-encoded raw transaction with signature(s)"},
                    {RPCResult::Type::STR, "psbt", /*optional=*/true, "If more signatures are needed, or if add_to_wallet is false, the base64-encoded (partially) signed transaction"}
                }
        },
        RPCExamples{""
        "\nSpend all UTXOs from the wallet with a fee rate of 1 " + CURRENCY_ATOM + "/vB using named arguments\n"
        + HelpExampleCli("-named sendall", "recipients='[\"" + EXAMPLE_ADDRESS[0] + "\"]' fee_rate=1\n") +
        "Spend all UTXOs with a fee rate of 1.1 " + CURRENCY_ATOM + "/vB using positional arguments\n"
        + HelpExampleCli("sendall", "'[\"" + EXAMPLE_ADDRESS[0] + "\"]' null \"unset\" 1.1\n") +
        "Spend all UTXOs split into equal amounts to two addresses with a fee rate of 1.5 " + CURRENCY_ATOM + "/vB using the options argument\n"
        + HelpExampleCli("sendall", "'[\"" + EXAMPLE_ADDRESS[0] + "\", \"" + EXAMPLE_ADDRESS[1] + "\"]' null \"unset\" null '{\"fee_rate\": 1.5}'\n") +
        "Leave dust UTXOs in wallet, spend only UTXOs with positive effective value with a fee rate of 10 " + CURRENCY_ATOM + "/vB using the options argument\n"
        + HelpExampleCli("sendall", "'[\"" + EXAMPLE_ADDRESS[0] + "\"]' null \"unset\" null '{\"fee_rate\": 10, \"send_max\": true}'\n") +
        "Spend all UTXOs with a fee rate of 1.3 " + CURRENCY_ATOM + "/vB using named arguments and sending a 0.25 " + CURRENCY_UNIT + " to another recipient\n"
        + HelpExampleCli("-named sendall", "recipients='[{\"" + EXAMPLE_ADDRESS[1] + "\": 0.25}, \""+ EXAMPLE_ADDRESS[0] + "\"]' fee_rate=1.3\n")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            std::shared_ptr<CWallet> const pwallet{GetWalletForJSONRPCRequest(request)};
            if (!pwallet) return UniValue::VNULL;
            // Make sure the results are valid at least up to the most recent block
            // the user could have gotten from another RPC command prior to now
            pwallet->BlockUntilSyncedToCurrentChain();

            UniValue options{request.params[4].isNull() ? UniValue::VOBJ : request.params[4]};
            InterpretFeeEstimationInstructions(/*conf_target=*/request.params[1], /*estimate_mode=*/request.params[2], /*fee_rate=*/request.params[3], options);
            PreventOutdatedOptions(options);


            std::set<std::string> addresses_without_amount;
            UniValue recipient_key_value_pairs(UniValue::VARR);
            const UniValue& recipients{request.params[0]};
            for (unsigned int i = 0; i < recipients.size(); ++i) {
                const UniValue& recipient{recipients[i]};
                if (recipient.isStr()) {
                    UniValue rkvp(UniValue::VOBJ);
                    rkvp.pushKV(recipient.get_str(), 0);
                    recipient_key_value_pairs.push_back(rkvp);
                    addresses_without_amount.insert(recipient.get_str());
                } else {
                    recipient_key_value_pairs.push_back(recipient);
                }
            }

            if (addresses_without_amount.size() == 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Must provide at least one address without a specified amount");
            }

            CCoinControl coin_control;

            coin_control.fAllowWatchOnly = ParseIncludeWatchonly(options["include_watching"], *pwallet);

            if (options.exists("minconf")) {
                if (options["minconf"].getInt<int>() < 0)
                {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid minconf (minconf cannot be negative): %s", options["minconf"].getInt<int>()));
                }

                coin_control.m_min_depth = options["minconf"].getInt<int>();
            }

            if (options.exists("maxconf")) {
                coin_control.m_max_depth = options["maxconf"].getInt<int>();

                if (coin_control.m_max_depth < coin_control.m_min_depth) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("maxconf can't be lower than minconf: %d < %d", coin_control.m_max_depth, coin_control.m_min_depth));
                }
            }

            unsigned int nTime = TicksSinceEpoch<std::chrono::seconds>(GetAdjustedTime());
            CMutableTransaction rawTx{ConstructTransaction(options["inputs"], recipient_key_value_pairs, options["locktime"], nTime)};
            LOCK(pwallet->cs_wallet);

            CAmount total_input_value(0);
            bool send_max{options.exists("send_max") ? options["send_max"].get_bool() : false};
            if (options.exists("inputs") && options.exists("send_max")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot combine send_max with specific inputs.");
            } else if (options.exists("inputs") && (options.exists("minconf") || options.exists("maxconf"))) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot combine minconf or maxconf with specific inputs.");
            } else if (options.exists("inputs")) {
                for (const CTxIn& input : rawTx.vin) {
                    if (pwallet->IsSpent(input.prevout)) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Input not available. UTXO (%s:%d) was already spent.", input.prevout.hash.ToString(), input.prevout.n));
                    }
                    const CWalletTx* tx{pwallet->GetWalletTx(input.prevout.hash)};
                    if (!tx || input.prevout.n >= tx->tx->vout.size() || !(pwallet->IsMine(tx->tx->vout[input.prevout.n]) & (coin_control.fAllowWatchOnly ? ISMINE_ALL : ISMINE_SPENDABLE))) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Input not found. UTXO (%s:%d) is not part of wallet.", input.prevout.hash.ToString(), input.prevout.n));
                    }
                    total_input_value += tx->tx->vout[input.prevout.n].nValue;
                }
            } else {
                CoinFilterParams coins_params;
                coins_params.min_amount = 0;
                for (const COutput& output : AvailableCoins(*pwallet, &coin_control).All()) {
                    CHECK_NONFATAL(output.input_bytes > 0);
                    CTxIn input(output.outpoint.hash, output.outpoint.n, CScript(), CTxIn::SEQUENCE_FINAL);
                    rawTx.vin.push_back(input);
                    total_input_value += output.txout.nValue;
                }
            }

            // estimate final size of tx
            const TxSize tx_size{CalculateMaximumSignedTxSize(CTransaction(rawTx), pwallet.get())};
            const CAmount fee_from_size{GetMinFee(tx_size.vsize, nTime)};
            const CAmount effective_value{total_input_value - fee_from_size};

            if (effective_value <= 0) {
                if (send_max) {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Total value of UTXO pool too low to pay for transaction, try using lower feerate.");
                } else {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Total value of UTXO pool too low to pay for transaction. Try using lower feerate or excluding uneconomic UTXOs with 'send_max' option.");
                }
            }

            // If this transaction is too large, e.g. because the wallet has many UTXOs, it will be rejected by the node's mempool.
            if (tx_size.weight > MAX_STANDARD_TX_WEIGHT) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Transaction too large.");
            }

            CAmount output_amounts_claimed{0};
            for (const CTxOut& out : rawTx.vout) {
                output_amounts_claimed += out.nValue;
            }

            if (output_amounts_claimed > total_input_value) {
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Assigned more value to outputs than available funds.");
            }

            const CAmount remainder{effective_value - output_amounts_claimed};
            if (remainder < 0) {
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds for fees after creating specified outputs.");
            }

            const CAmount per_output_without_amount{remainder / (long)addresses_without_amount.size()};

            bool gave_remaining_to_first{false};
            for (CTxOut& out : rawTx.vout) {
                CTxDestination dest;
                ExtractDestination(out.scriptPubKey, dest);
                std::string addr{EncodeDestination(dest)};
                if (addresses_without_amount.count(addr) > 0) {
                    out.nValue = per_output_without_amount;
                    if (!gave_remaining_to_first) {
                        out.nValue += remainder % addresses_without_amount.size();
                        gave_remaining_to_first = true;
                    }
                    /*if (IsDust(out, pwallet->chain().relayDustFee())) {
                        // Dynamically generated output amount is dust
                        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Dynamically assigned remainder results in dust output.");
                    }*/
                }/* else {
                    if (IsDust(out, pwallet->chain().relayDustFee())) {
                        // Specified output amount is dust
                        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Specified output amount to %s is below dust threshold.", addr));
                    }
                }*/
            }

            const bool lock_unspents{options.exists("lock_unspents") ? options["lock_unspents"].get_bool() : false};
            if (lock_unspents) {
                for (const CTxIn& txin : rawTx.vin) {
                    pwallet->LockCoin(txin.prevout);
                }
            }

            return FinishTransaction(pwallet, options, rawTx);
        }
    };
}

RPCHelpMan walletprocesspsbt()
{
    return RPCHelpMan{"walletprocesspsbt",
                "\nUpdate a PSBT with input information from our wallet and then sign inputs\n"
                "that we can sign for." +
        HELP_REQUIRING_PASSPHRASE,
                {
                    {"psbt", RPCArg::Type::STR, RPCArg::Optional::NO, "The transaction base64 string"},
                    {"sign", RPCArg::Type::BOOL, RPCArg::Default{true}, "Also sign the transaction when updating (requires wallet to be unlocked)"},
                    {"sighashtype", RPCArg::Type::STR, RPCArg::Default{"DEFAULT for Taproot, ALL otherwise"}, "The signature hash type to sign with if not specified by the PSBT. Must be one of\n"
            "       \"DEFAULT\"\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\""},
                    {"bip32derivs", RPCArg::Type::BOOL, RPCArg::Default{true}, "Include BIP 32 derivation paths for public keys if we know them"},
                    {"finalize", RPCArg::Type::BOOL, RPCArg::Default{true}, "Also finalize inputs if possible"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "psbt", "The base64-encoded partially signed transaction"},
                        {RPCResult::Type::BOOL, "complete", "If the transaction has a complete set of signatures"},
                    }
                },
                RPCExamples{
                    HelpExampleCli("walletprocesspsbt", "\"psbt\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<const CWallet> pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;

    const CWallet& wallet{*pwallet};
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    // Unserialize the transaction
    PartiallySignedTransaction psbtx;
    std::string error;
    if (!DecodeBase64PSBT(psbtx, request.params[0].get_str(), error)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("TX decode failed %s", error));
    }

    // Get the sighash type
    int nHashType = ParseSighashString(request.params[2]);

    // Fill transaction with our data and also sign
    bool sign = request.params[1].isNull() ? true : request.params[1].get_bool();
    bool bip32derivs = request.params[3].isNull() ? true : request.params[3].get_bool();
    bool finalize = request.params[4].isNull() ? true : request.params[4].get_bool();
    bool complete = true;

    if (sign) EnsureWalletIsUnlocked(*pwallet);

    const TransactionError err{wallet.FillPSBT(psbtx, complete, nHashType, sign, bip32derivs, nullptr, finalize)};
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }

    UniValue result(UniValue::VOBJ);
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;
    result.pushKV("psbt", EncodeBase64(ssTx.str()));
    result.pushKV("complete", complete);

    return result;
},
    };
}

RPCHelpMan walletcreatefundedpsbt()
{
    return RPCHelpMan{"walletcreatefundedpsbt",
                "\nCreates and funds a transaction in the Partially Signed Transaction format.\n"
                "Implements the Creator and Updater roles.\n"
                "All existing inputs must either have their previous output transaction be in the wallet\n"
                "or be in the UTXO set. Solving data must be provided for non-wallet inputs.\n",
                {
                    {"inputs", RPCArg::Type::ARR, RPCArg::Optional::OMITTED, "Leave empty to add inputs automatically. See add_inputs option.",
                        {
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id"},
                                    {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The output number"},
                                    {"sequence", RPCArg::Type::NUM, RPCArg::DefaultHint{"depends on the value of the 'locktime' and 'options.replaceable' arguments"}, "The sequence number"},
                                    {"weight", RPCArg::Type::NUM, RPCArg::DefaultHint{"Calculated from wallet and solving data"}, "The maximum weight for this input, "
                                        "including the weight of the outpoint and sequence number. "
                                        "Note that signature sizes are not guaranteed to be consistent, "
                                        "so the maximum DER signatures size of 73 bytes should be used when considering ECDSA signatures."
                                        "Remember to convert serialized sizes to weight units when necessary."},
                                },
                            },
                        },
                        },
                    {"outputs", RPCArg::Type::ARR, RPCArg::Optional::NO, "The outputs (key-value pairs), where none of the keys are duplicated.\n"
                            "That is, each address can only appear once and there can only be one 'data' object.\n"
                            "For compatibility reasons, a dictionary, which holds the key-value pairs directly, is also\n"
                            "accepted as second parameter.",
                        {
                            {"", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::OMITTED, "",
                                {
                                    {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A key-value pair. The key (string) is the Brycecoin address, the value (float or string) is the amount in " + CURRENCY_UNIT + ""},
                                },
                                },
                            {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                                {
                                    {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "A key-value pair. The key must be \"data\", the value is hex-encoded data"},
                                },
                            },
                        },
                     RPCArgOptions{.skip_type_check = true}},
                    {"locktime", RPCArg::Type::NUM, RPCArg::Default{0}, "Raw locktime. Non-0 value also locktime-activates inputs"},
                    {"timestamp", RPCArg::Type::NUM, RPCArg::Default{0}, "Transaction timestamp"},
                    {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        Cat<std::vector<RPCArg>>(
                        {
                            {"add_inputs", RPCArg::Type::BOOL, RPCArg::DefaultHint{"false when \"inputs\" are specified, true otherwise"}, "Automatically include coins from the wallet to cover the target amount.\n"},
                            {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include inputs that are not safe to spend (unconfirmed transactions from outside keys and unconfirmed replacement transactions).\n"
                                                          "Warning: the resulting transaction may become invalid if one of the unsafe inputs disappears.\n"
                                                          "If that happens, you will need to fund the transaction with different inputs and republish it."},
                            {"changeAddress", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"pool address"}, "The Brycecoin address to receive the change"},
                            {"changePosition", RPCArg::Type::NUM, RPCArg::DefaultHint{"random"}, "The index of the change output"},
                            {"change_type", RPCArg::Type::STR, RPCArg::DefaultHint{"set by -changetype"}, "The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", \"bech32\", and \"bech32m\"."},
                            {"includeWatching", RPCArg::Type::BOOL, RPCArg::DefaultHint{"true for watch-only wallets, otherwise false"}, "Also select inputs which are watch only"},
                            {"lockUnspents", RPCArg::Type::BOOL, RPCArg::Default{false}, "Lock selected unspent outputs"},
                            {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_ATOM + "/vB."},
                            {"feeRate", RPCArg::Type::AMOUNT, RPCArg::DefaultHint{"not set, fall back to wallet fee estimation"}, "Specify a fee rate in " + CURRENCY_UNIT + "/kvB."},
                            {"subtractFeeFromOutputs", RPCArg::Type::ARR, RPCArg::Default{UniValue::VARR}, "The outputs to subtract the fee from.\n"
                                                          "The fee will be equally deducted from the amount of each specified output.\n"
                                                          "Those recipients will receive less Brycecoins than you enter in their corresponding amount field.\n"
                                                          "If no outputs are specified here, the sender pays the fee.",
                                {
                                    {"vout_index", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "The zero-based output index, before a change output is added."},
                                },
                            },
                        },
                        FundTxDoc()),
                        RPCArgOptions{.oneline_description="options"}},
                    {"bip32derivs", RPCArg::Type::BOOL, RPCArg::Default{true}, "Include BIP 32 derivation paths for public keys if we know them"},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR, "psbt", "The resulting raw transaction (base64-encoded string)"},
                        {RPCResult::Type::STR_AMOUNT, "fee", "Fee in " + CURRENCY_UNIT + " the resulting transaction pays"},
                        {RPCResult::Type::NUM, "changepos", "The position of the added change output, or -1"},
                    }
                                },
                                RPCExamples{
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("walletcreatefundedpsbt", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"[{\\\"data\\\":\\\"00010203\\\"}]\"")
                                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;

    CWallet& wallet{*pwallet};
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    UniValue options{request.params[3].isNull() ? UniValue::VOBJ : request.params[3]};

    CAmount fee;
    int change_position;
    CMutableTransaction rawTx = ConstructTransaction(request.params[0], request.params[1], request.params[2], request.params[3]);
    CCoinControl coin_control;
    // Automatically select coins, unless at least one is manually selected. Can
    // be overridden by options.add_inputs.
    coin_control.m_allow_other_inputs = rawTx.vin.size() == 0;
    SetOptionsInputWeights(request.params[0], options);
    FundTransaction(wallet, rawTx, fee, change_position, request.params[4], coin_control, /* override_min_fee */ true);

    // Make a blank psbt
    PartiallySignedTransaction psbtx(rawTx);

    // Fill transaction with out data but don't sign
    bool bip32derivs = request.params[5].isNull() ? true : request.params[5].get_bool();
    bool complete = true;
    const TransactionError err{wallet.FillPSBT(psbtx, complete, 1, /*sign=*/false, /*bip32derivs=*/bip32derivs)};
    if (err != TransactionError::OK) {
        throw JSONRPCTransactionError(err);
    }

    // Serialize the PSBT
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << psbtx;

    UniValue result(UniValue::VOBJ);
    result.pushKV("psbt", EncodeBase64(ssTx.str()));
    result.pushKV("fee", ValueFromAmount(fee));
    result.pushKV("changepos", change_position);
    return result;
},
    };
}
} // namespace wallet
