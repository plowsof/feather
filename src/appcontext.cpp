// SPDX-License-Identifier: BSD-3-Clause
// SPDX-FileCopyrightText: 2020-2022 The Monero Project

#include <QDir>

#include "appcontext.h"
#include "constants.h"

// libwalletqt
#include "libwalletqt/TransactionHistory.h"
#include "libwalletqt/Subaddress.h"
#include "libwalletqt/Coins.h"
#include "model/TransactionHistoryModel.h"
#include "model/SubaddressModel.h"
#include "utils/NetworkManager.h"
#include "utils/WebsocketClient.h"
#include "utils/WebsocketNotifier.h"

// This class serves as a business logic layer between MainWindow and libwalletqt.
// This way we don't clutter the GUI with wallet logic,
// and keep libwalletqt (mostly) clean of Feather specific implementation details

AppContext::AppContext(Wallet *wallet)
    : wallet(wallet)
    , nodes(new Nodes(this, this))
    , networkType(constants::networkType)
    , m_rpc(new DaemonRpc{this, getNetworkTor(), ""})
{
    connect(this->wallet, &Wallet::moneySpent,               this, &AppContext::onMoneySpent);
    connect(this->wallet, &Wallet::moneyReceived,            this, &AppContext::onMoneyReceived);
    connect(this->wallet, &Wallet::unconfirmedMoneyReceived, this, &AppContext::onUnconfirmedMoneyReceived);
    connect(this->wallet, &Wallet::newBlock,                 this, &AppContext::onWalletNewBlock);
    connect(this->wallet, &Wallet::updated,                  this, &AppContext::onWalletUpdate);
    connect(this->wallet, &Wallet::refreshed,                this, &AppContext::onWalletRefreshed);
    connect(this->wallet, &Wallet::transactionCommitted,     this, &AppContext::onTransactionCommitted);
    connect(this->wallet, &Wallet::heightRefreshed,          this, &AppContext::onHeightRefreshed);
    connect(this->wallet, &Wallet::transactionCreated,       this, &AppContext::onTransactionCreated);
    connect(this->wallet, &Wallet::deviceError,              this, &AppContext::onDeviceError);
    connect(this->wallet, &Wallet::deviceButtonRequest,      this, &AppContext::onDeviceButtonRequest);
    connect(this->wallet, &Wallet::deviceButtonPressed,      this, &AppContext::onDeviceButtonPressed);
    connect(this->wallet, &Wallet::connectionStatusChanged, [this]{
        this->nodes->autoConnect();
    });
    connect(this->wallet, &Wallet::currentSubaddressAccountChanged, [this]{
        this->updateBalance();
    });

    connect(this, &AppContext::createTransactionError, this, &AppContext::onCreateTransactionError);

    // Store the wallet every 2 minutes
    m_storeTimer.start(2 * 60 * 1000);
    connect(&m_storeTimer, &QTimer::timeout, [this](){
        this->storeWallet();
    });

    this->updateBalance();

    connect(this->wallet->history(), &TransactionHistory::txNoteChanged, [this]{
        this->wallet->history()->refresh(this->wallet->currentSubaddressAccount());
    });
}

// ################## Transaction creation ##################

void AppContext::onCreateTransaction(const QString &address, quint64 amount, const QString &description, bool all) {
    this->tmpTxDescription = description;

    if (!all && amount == 0) {
        emit createTransactionError("Cannot send nothing");
        return;
    }

    quint64 unlocked_balance = this->wallet->unlockedBalance();
    if (!all && amount > unlocked_balance) {
        emit createTransactionError(QString("Not enough money to spend.\n\n"
                                            "Spendable balance: %1").arg(WalletManager::displayAmount(unlocked_balance)));
        return;
    } else if (unlocked_balance == 0) {
        emit createTransactionError("No money to spend");
        return;
    }

    qInfo() << "Creating transaction";
    if (all)
        this->wallet->createTransactionAllAsync(address, "", constants::mixin, this->tx_priority, m_selectedInputs);
    else
        this->wallet->createTransactionAsync(address, "", amount, constants::mixin, this->tx_priority, m_selectedInputs);

    emit initiateTransaction();
}

void AppContext::onCreateTransactionMultiDest(const QVector<QString> &addresses, const QVector<quint64> &amounts, const QString &description) {
    this->tmpTxDescription = description;

    quint64 total_amount = 0;
    for (auto &amount : amounts) {
        total_amount += amount;
    }

    auto unlocked_balance = this->wallet->unlockedBalance();
    if (total_amount > unlocked_balance) {
        emit createTransactionError("Not enough money to spend");
    }

    qInfo() << "Creating transaction";
    this->wallet->createTransactionMultiDestAsync(addresses, amounts, this->tx_priority, m_selectedInputs);

    emit initiateTransaction();
}

void AppContext::onSweepOutputs(const QVector<QString> &keyImages, QString address, bool churn, int outputs) {
    if (churn) {
        address = this->wallet->address(0, 0);
    }

    qInfo() << "Creating transaction";
    this->wallet->createTransactionSelectedAsync(keyImages, address, outputs, this->tx_priority);

    emit initiateTransaction();
}

void AppContext::onCreateTransactionError(const QString &msg) {
    this->tmpTxDescription = "";
    emit endTransaction();
}

void AppContext::onCancelTransaction(PendingTransaction *tx, const QVector<QString> &address) {
    // tx cancelled by user
    emit createTransactionCancelled(address, tx->amount());
    this->wallet->disposeTransaction(tx);
}

void AppContext::commitTransaction(PendingTransaction *tx, const QString &description) {
    // Clear list of selected transfers
    this->setSelectedInputs({});

    // Nodes - even well-connected, properly configured ones - consistently fail to relay transactions
    // To mitigate transactions failing we just send the transaction to every node we know about over Tor
    if (config()->get(Config::multiBroadcast).toBool()) {
        this->onMultiBroadcast(tx);
    }

    this->wallet->commitTransactionAsync(tx, description);
}

void AppContext::onMultiBroadcast(PendingTransaction *tx) {
    quint64 count = tx->txCount();
    for (quint64 i = 0; i < count; i++) {
        QString txData = tx->signedTxToHex(i);

        for (const auto& node: this->nodes->nodes()) {
            QString address = node.toURL();
            qDebug() << QString("Relaying %1 to: %2").arg(tx->txid()[i], address);
            m_rpc->setDaemonAddress(address);
            m_rpc->sendRawTransaction(txData);
        }
    }
}

void AppContext::addCacheTransaction(const QString &txid, const QString &txHex) const {
    this->wallet->setCacheAttribute(QString("tx:%1").arg(txid), txHex);
}

QString AppContext::getCacheTransaction(const QString &txid) const {
    QString txHex = this->wallet->getCacheAttribute(QString("tx:%1").arg(txid));
    return txHex;
}

// ################## Device ##################

void AppContext::onDeviceButtonRequest(quint64 code) {
    emit deviceButtonRequest(code);
}

void AppContext::onDeviceButtonPressed() {
    emit deviceButtonPressed();
}

void AppContext::onDeviceError(const QString &message) {
    qCritical() << "Device error: " << message;
    emit deviceError(message);
}

// ################## Misc ##################

void AppContext::setSelectedInputs(const QStringList &selectedInputs) {
    m_selectedInputs = selectedInputs;
    emit selectedInputsChanged(selectedInputs);
}

void AppContext::onTorSettingsChanged() {
    if (Utils::isTorsocks()) {
        return;
    }

    this->nodes->connectToNode();

    auto privacyLevel = config()->get(Config::torPrivacyLevel).toInt();
    qDebug() << "Changed privacyLevel to " << privacyLevel;
}

void AppContext::onSetRestoreHeight(quint64 height){
    auto seed = this->wallet->getCacheAttribute("feather.seed");
    if(!seed.isEmpty()) {
        const auto msg = "This wallet has a 14 word mnemonic seed which has the restore height embedded.";
        emit setRestoreHeightError(msg);
        return;
    }

    this->wallet->setWalletCreationHeight(height);
    this->wallet->setPassword(this->wallet->getPassword());  // trigger .keys write

    // nuke wallet cache
    const auto fn = this->wallet->cachePath();
    WalletManager::clearWalletCache(fn);

    emit customRestoreHeightSet(height);
}

void AppContext::stopTimers() {
    m_storeTimer.stop();
}

// ########################################## LIBWALLET QT SIGNALS ####################################################

void AppContext::onMoneySpent(const QString &txId, quint64 amount) {
    // Outgoing tx included in a block
    qDebug() << Q_FUNC_INFO << txId << " " << WalletManager::displayAmount(amount);
}

void AppContext::onMoneyReceived(const QString &txId, quint64 amount) {
    // Incoming tx included in a block.
    qDebug() << Q_FUNC_INFO << txId << " " << WalletManager::displayAmount(amount);
}

void AppContext::onUnconfirmedMoneyReceived(const QString &txId, quint64 amount) {
    // Incoming tx in pool
    qDebug() << Q_FUNC_INFO << txId << " " << WalletManager::displayAmount(amount);

    if (this->wallet->synchronized()) {
        auto notify = QString("%1 XMR (pending)").arg(WalletManager::displayAmount(amount, false));
        Utils::desktopNotify("Payment received", notify, 5000);
    }
}

void AppContext::onWalletUpdate() {
    if (this->wallet->synchronized()) {
        this->refreshModels();
        this->storeWallet();
    }

    this->updateBalance();
}

void AppContext::onWalletRefreshed(bool success, const QString &message) {
    if (!success) {
        // Something went wrong during refresh, in some cases we need to notify the user
        qCritical() << "Exception during refresh: " << message; // Can't use ->errorString() here, other SLOT might snipe it first
        return;
    }

    if (!this->refreshed) {
        refreshModels();
        this->refreshed = true;
        emit walletRefreshed();
        // store wallet immediately upon finishing synchronization
        this->wallet->store();
    }
}

void AppContext::onWalletNewBlock(quint64 blockheight, quint64 targetHeight) {
    // Called whenever a new block gets scanned by the wallet
    this->syncStatusUpdated(blockheight, targetHeight);

    if (this->wallet->isSynchronized()) {
        this->wallet->coins()->refreshUnlocked();
        this->wallet->history()->refresh(this->wallet->currentSubaddressAccount());
        // Todo: only refresh tx confirmations
    }
}

void AppContext::onHeightRefreshed(quint64 walletHeight, quint64 daemonHeight, quint64 targetHeight) {
    if (this->wallet->connectionStatus() == Wallet::ConnectionStatus_Disconnected)
        return;

    if (daemonHeight < targetHeight) {
        emit blockchainSync(daemonHeight, targetHeight);
    }
    else {
        this->syncStatusUpdated(walletHeight, daemonHeight);
    }
}

void AppContext::onTransactionCreated(PendingTransaction *tx, const QVector<QString> &address) {
    qDebug() << Q_FUNC_INFO;

    for (auto &addr : address) {
        if (addr == constants::donationAddress) {
            this->donationSending = true;
        }
    }

    // Let UI know that the transaction was constructed
    emit endTransaction();

    // tx created, but not sent yet. ask user to verify first.
    emit createTransactionSuccess(tx, address);
}

void AppContext::onTransactionCommitted(bool status, PendingTransaction *tx, const QStringList& txid){
    // Store wallet immediately so we don't risk losing tx key if wallet crashes
    this->wallet->store();

    this->wallet->history()->refresh(this->wallet->currentSubaddressAccount());
    this->wallet->coins()->refresh(this->wallet->currentSubaddressAccount());

    this->updateBalance();

    // this tx was a donation to Feather, stop our nagging
    if (this->donationSending) {
        this->donationSending = false;
        config()->set(Config::donateBeg, -1);
    }

    emit transactionCommitted(status, tx, txid);
}

void AppContext::storeWallet() {
    // Do not store a synchronizing wallet: store() is NOT thread safe and may crash the wallet
    if (!this->wallet->isSynchronized())
        return;

    qDebug() << "Storing wallet";
    this->wallet->store();
}

void AppContext::updateBalance() {
    quint64 balance = this->wallet->balance();
    quint64 spendable = this->wallet->unlockedBalance();

    emit balanceUpdated(balance, spendable);
}

void AppContext::syncStatusUpdated(quint64 height, quint64 target) {
    if (height < (target - 1)) {
        emit refreshSync(height, target);
    }
    else {
        this->updateBalance();
        emit synchronized();
    }
}

void AppContext::refreshModels() {
    this->wallet->history()->refresh(this->wallet->currentSubaddressAccount());
    this->wallet->coins()->refresh(this->wallet->currentSubaddressAccount());
    bool r = this->wallet->subaddress()->refresh(this->wallet->currentSubaddressAccount());

    if (!r) {
        // This should only happen if wallet keys got corrupted or were tampered with
        // The list of subaddresses is wiped to prevent loss of funds
        // Notify MainWindow to display an error message
        emit keysCorrupted();
    }
}
