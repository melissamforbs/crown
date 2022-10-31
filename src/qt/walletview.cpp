// Copyright (c) 2011-2020 The Crown Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/walletview.h>

#include <qt/addressbookpage.h>
#include <qt/askpassphrasedialog.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/psbtoperationsdialog.h>
#include <qt/optionsmodel.h>
#include <qt/overviewpage.h>
#include <qt/platformstyle.h>
#include <qt/receivecoinsdialog.h>
#include <qt/sendcoinsdialog.h>
#include <qt/signverifymessagedialog.h>
#include <qt/transactiontablemodel.h>
#include <qt/transactionview.h>
#include <qt/walletmodel.h>
#include <qt/multisigdialog.h>
#include <qt/transactionrecord.h>
#include <qt/createsystemnodedialog.h>
#include <qt/createmasternodedialog.h>

#include <interfaces/node.h>
#include <node/ui_interface.h>
#include <psbt.h>
#include <util/strencodings.h>

#include <masternode/masternodeconfig.h>
#include <systemnode/systemnodeconfig.h>

#include <wallet/wallet.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QProgressDialog>
#include <QPushButton>
#include <QVBoxLayout>

#include <fstream>

WalletView::WalletView(const PlatformStyle *_platformStyle, QWidget *parent):
    QStackedWidget(parent),
    clientModel(nullptr),
    walletModel(nullptr),
    platformStyle(_platformStyle)
{
    // Create tabs
    overviewPage = new OverviewPage(platformStyle);

    transactionsPage = new QWidget(this);
    QVBoxLayout *vbox = new QVBoxLayout();
    QHBoxLayout *hbox_buttons = new QHBoxLayout();
    transactionView = new TransactionView(platformStyle, this);
    vbox->addWidget(transactionView);
    QPushButton *exportButton = new QPushButton(tr("&Export"), this);
    exportButton->setToolTip(tr("Export the data in the current tab to a file"));
    if (platformStyle->getImagesOnButtons()) {
        exportButton->setIcon(platformStyle->SingleColorIcon(":/icons/export"));
    }
    hbox_buttons->addStretch();
    hbox_buttons->addWidget(exportButton);
    vbox->addLayout(hbox_buttons);
    transactionsPage->setLayout(vbox);

    receiveCoinsPage = new ReceiveCoinsDialog(platformStyle);
    sendCoinsPage = new SendCoinsDialog(platformStyle);

    usedSendingAddressesPage = new AddressBookPage(platformStyle, AddressBookPage::ForEditing, AddressBookPage::SendingTab, this);
    usedReceivingAddressesPage = new AddressBookPage(platformStyle, AddressBookPage::ForEditing, AddressBookPage::ReceivingTab, this);
    masternodeListPage = new MasternodeList(platformStyle);
    systemnodeListPage = new SystemnodeList(platformStyle);

    addWidget(overviewPage);
    addWidget(transactionsPage);
    addWidget(receiveCoinsPage);
    addWidget(sendCoinsPage);
    addWidget(masternodeListPage);
    addWidget(systemnodeListPage);

    connect(overviewPage, &OverviewPage::transactionClicked, this, &WalletView::transactionClicked);
    // Clicking on a transaction on the overview pre-selects the transaction on the transaction history page
    connect(overviewPage, &OverviewPage::transactionClicked, transactionView, static_cast<void (TransactionView::*)(const QModelIndex&)>(&TransactionView::focusTransaction));

    connect(overviewPage, &OverviewPage::outOfSyncWarningClicked, this, &WalletView::requestedSyncWarningInfo);

    connect(sendCoinsPage, &SendCoinsDialog::coinsSent, this, &WalletView::coinsSent);
    // Highlight transaction after send
    connect(sendCoinsPage, &SendCoinsDialog::coinsSent, transactionView, static_cast<void (TransactionView::*)(const uint256&)>(&TransactionView::focusTransaction));

    // Clicking on "Export" allows to export the transaction list
    connect(exportButton, &QPushButton::clicked, transactionView, &TransactionView::exportClicked);

    // Pass through messages from sendCoinsPage
    connect(sendCoinsPage, &SendCoinsDialog::message, this, &WalletView::message);
    // Pass through messages from transactionView
    connect(transactionView, &TransactionView::message, this, &WalletView::message);

    connect(this, &WalletView::setPrivacy, overviewPage, &OverviewPage::setPrivacy);
}

WalletView::~WalletView()
{
}

void WalletView::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    overviewPage->setClientModel(_clientModel);
    sendCoinsPage->setClientModel(_clientModel);
    masternodeListPage->setClientModel(_clientModel);
    systemnodeListPage->setClientModel(_clientModel);
    if (walletModel) walletModel->setClientModel(_clientModel);
}

void WalletView::setWalletModel(WalletModel *_walletModel)
{
    this->walletModel = _walletModel;

    // Put transaction list in tabs
    transactionView->setModel(_walletModel);
    overviewPage->setWalletModel(_walletModel);
    receiveCoinsPage->setModel(_walletModel);
    sendCoinsPage->setModel(_walletModel);
    usedReceivingAddressesPage->setModel(_walletModel ? _walletModel->getAddressTableModel() : nullptr);
    usedSendingAddressesPage->setModel(_walletModel ? _walletModel->getAddressTableModel() : nullptr);
    masternodeListPage->setWalletModel(_walletModel);
    systemnodeListPage->setWalletModel(_walletModel);
    if (_walletModel)
    {
        // Receive and pass through messages from wallet model
        connect(_walletModel, &WalletModel::message, this, &WalletView::message);

        // Handle changes in encryption status
        connect(_walletModel, &WalletModel::encryptionStatusChanged, this, &WalletView::encryptionStatusChanged);
        updateEncryptionStatus();

        // update HD status
        Q_EMIT hdEnabledStatusChanged();

        // Balloon pop-up for new transaction
        connect(_walletModel->getTransactionTableModel(), &TransactionTableModel::rowsInserted, this, &WalletView::processNewTransaction);

        // Ask for passphrase if needed
        connect(_walletModel, &WalletModel::requireUnlock, this, &WalletView::unlockWallet);

        // Show progress dialog
        connect(_walletModel, &WalletModel::showProgress, this, &WalletView::showProgress);
    }
}

void WalletView::addSystemnode(CNodeEntry nodeEntry)
{
    systemnodeConfig.add(nodeEntry);
    systemnodeConfig.write();
    Q_EMIT gotoSystemnodePage();
    systemnodeListPage->updateMyNodeList(true);
    systemnodeListPage->selectAliasRow(QString::fromStdString(nodeEntry.getAlias()));
}

void WalletView::addMasternode(CNodeEntry nodeEntry)
{
    masternodeConfig.add(nodeEntry);
    masternodeConfig.write();
    Q_EMIT gotoMasternodePage();
    masternodeListPage->updateMyNodeList(true);
    masternodeListPage->selectAliasRow(QString::fromStdString(nodeEntry.getAlias()));
}

void WalletView::checkAndCreateNode(const COutput& out)
{
    bool systemnodePayment = false;
    bool masternodePayment = false;
    //CAmount credit = out.tx->vout[out.i].nValue;
    CAmount credit = (out.tx->tx->nVersion >= TX_ELE_VERSION ? out.tx->tx->vpout[out.i].nValue : out.tx->tx->vout[out.i].nValue);

    if (credit == SYSTEMNODE_COLLATERAL * COIN) {
        systemnodePayment = true;
    } else if (credit == MASTERNODE_COLLATERAL * COIN) {
        masternodePayment = true;
    } else {
        return;
    }

    CreateNodeDialog *dialog = new CreateSystemnodeDialog(this);
    QString title = tr("Payment to yourself - ") +
        CrownUnits::formatWithUnit(walletModel->getOptionsModel()->getDisplayUnit(), credit);
    QString body = tr("Do you want to create a new ");
    if (systemnodePayment)
    {
        if (systemnodeListPage->getSendCollateralDialog()->fAutoCreate) {
            return;
        }
        body += "Systemnode?";
        dialog = new CreateSystemnodeDialog(this);
    } else if (masternodePayment) {
        if (masternodeListPage->getSendCollateralDialog()->fAutoCreate) {
            return;
        }
        body += "Masternode?";
        dialog = new CreateMasternodeDialog(this);
    }
    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this, title, body,
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);

    if (retval == QMessageBox::Yes)
    {
        dialog->setWindowModality(Qt::ApplicationModal);
        dialog->setEditMode();

        std::string port = "9340";
        if (Params().NetworkIDString() == CBaseChainParams::TESTNET) {
            port = "19340";
        }
        if (dialog->exec())
        {
            std::string alias = dialog->getAlias().toStdString();
            std::string ip = dialog->getIP().toStdString() + ":" + port;
            uint256 hash = out.tx->GetHash();
            COutPoint outpoint = COutPoint(hash, out.i);
            wallets[0]->LockCoin(outpoint);

            // Generate a key
            CKey secret;
            secret.MakeNewKey(false);
            std::string privateKey = CBitcoinSecret(secret).ToString();
            CNodeEntry entry(alias, ip, privateKey, hash.ToString(), strprintf("%d", out.i));
            if (systemnodePayment) {
                addSystemnode(entry);
            } else if (masternodePayment) {
                addMasternode(entry);
            }
        }
    }
}

void WalletView::processNewTransaction(const QModelIndex& parent, int start, int /*end*/)
{
    // Prevent balloon-spam when initial block download is in progress
    if (!walletModel || !clientModel || clientModel->node().isInitialBlockDownload())
        return;

    TransactionTableModel *ttm = walletModel->getTransactionTableModel();
    if (!ttm || ttm->processingQueuedTransactions())
        return;

    QString date = ttm->index(start, TransactionTableModel::Date, parent).data().toString();
    qint64 amount = ttm->index(start, TransactionTableModel::Amount, parent).data(Qt::EditRole).toULongLong();
    QString type = ttm->index(start, TransactionTableModel::Type, parent).data().toString();
    QModelIndex index = ttm->index(start, 0, parent);
    QString address = ttm->data(index, TransactionTableModel::AddressRole).toString();
    QString label = GUIUtil::HtmlEscape(ttm->data(index, TransactionTableModel::LabelRole).toString());

    Q_EMIT incomingTransaction(date, walletModel->getOptionsModel()->getDisplayUnit(), amount, type, address, label, GUIUtil::HtmlEscape(walletModel->getWalletName()));

    int typeEnum = ttm->index(start, 0, parent).data(TransactionTableModel::TypeRole).toInt();
    if (typeEnum == TransactionRecord::SendToSelf)
    {
        uint256 hash = ttm->index(start, 0, parent).data(TransactionTableModel::TxHashRole).value<uint256>();
        std::optional<COutput> res = pwalletMain->FindCollateralOutput(hash);
        if (res) {
            COutput out = res.get();
            checkAndCreateNode(out);
        }
    }
}

void WalletView::gotoOverviewPage()
{
    setCurrentWidget(overviewPage);
}

void WalletView::gotoHistoryPage()
{
    setCurrentWidget(transactionsPage);
}

void WalletView::gotoMasternodePage()
{
    setCurrentWidget(masternodeListPage);
}

void WalletView::gotoSystemnodePage()
{
    setCurrentWidget(systemnodeListPage);
}

void WalletView::gotoReceiveCoinsPage()
{
    setCurrentWidget(receiveCoinsPage);
}

void WalletView::gotoSendCoinsPage(QString addr)
{
    setCurrentWidget(sendCoinsPage);

    if (!addr.isEmpty())
        sendCoinsPage->setAddress(addr);
}

void WalletView::gotoMultisigTab()
{
    // calls show() in showTab_SM()
    MultisigDialog *multisigDialog = new MultisigDialog(this);
    multisigDialog->setAttribute(Qt::WA_DeleteOnClose);
    multisigDialog->setModel(walletModel);
    multisigDialog->showTab(true);
}

void WalletView::gotoSignMessageTab(QString addr)
{
    // calls show() in showTab_SM()
    SignVerifyMessageDialog *signVerifyMessageDialog = new SignVerifyMessageDialog(platformStyle, this);
    signVerifyMessageDialog->setAttribute(Qt::WA_DeleteOnClose);
    signVerifyMessageDialog->setModel(walletModel);
    signVerifyMessageDialog->showTab_SM(true);

    if (!addr.isEmpty())
        signVerifyMessageDialog->setAddress_SM(addr);
}

void WalletView::gotoVerifyMessageTab(QString addr)
{
    // calls show() in showTab_VM()
    SignVerifyMessageDialog *signVerifyMessageDialog = new SignVerifyMessageDialog(platformStyle, this);
    signVerifyMessageDialog->setAttribute(Qt::WA_DeleteOnClose);
    signVerifyMessageDialog->setModel(walletModel);
    signVerifyMessageDialog->showTab_VM(true);

    if (!addr.isEmpty())
        signVerifyMessageDialog->setAddress_VM(addr);
}

void WalletView::gotoLoadPSBT(bool from_clipboard)
{
    std::string data;

    if (from_clipboard) {
        std::string raw = QApplication::clipboard()->text().toStdString();
        bool invalid;
        data = DecodeBase64(raw, &invalid);
        if (invalid) {
            Q_EMIT message(tr("Error"), tr("Unable to decode PSBT from clipboard (invalid base64)"), CClientUIInterface::MSG_ERROR);
            return;
        }
    } else {
        QString filename = GUIUtil::getOpenFileName(this,
            tr("Load Transaction Data"), QString(),
            tr("Partially Signed Transaction (*.psbt)"), nullptr);
        if (filename.isEmpty()) return;
        if (GetFileSize(filename.toLocal8Bit().data(), MAX_FILE_SIZE_PSBT) == MAX_FILE_SIZE_PSBT) {
            Q_EMIT message(tr("Error"), tr("PSBT file must be smaller than 100 MiB"), CClientUIInterface::MSG_ERROR);
            return;
        }
        std::ifstream in(filename.toLocal8Bit().data(), std::ios::binary);
        data = std::string(std::istreambuf_iterator<char>{in}, {});
    }

    std::string error;
    PartiallySignedTransaction psbtx;
    if (!DecodeRawPSBT(psbtx, data, error)) {
        Q_EMIT message(tr("Error"), tr("Unable to decode PSBT") + "\n" + QString::fromStdString(error), CClientUIInterface::MSG_ERROR);
        return;
    }

    PSBTOperationsDialog* dlg = new PSBTOperationsDialog(this, walletModel, clientModel);
    dlg->openWithPSBT(psbtx);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

bool WalletView::handlePaymentRequest(const SendAssetsRecipient& recipient)
{
    return sendCoinsPage->handlePaymentRequest(recipient);
}

void WalletView::showOutOfSyncWarning(bool fShow)
{
    overviewPage->showOutOfSyncWarning(fShow);
}

void WalletView::updateEncryptionStatus()
{
    Q_EMIT encryptionStatusChanged();
}

void WalletView::encryptWallet(bool status)
{
    if(!walletModel)
        return;
    AskPassphraseDialog dlg(status ? AskPassphraseDialog::Encrypt : AskPassphraseDialog::Decrypt, this);
    dlg.setModel(walletModel);
    dlg.exec();

    updateEncryptionStatus();
}

void WalletView::backupWallet()
{
    QString filename = GUIUtil::getSaveFileName(this,
        tr("Backup Wallet"), QString(),
        tr("Wallet Data (*.dat)"), nullptr);

    if (filename.isEmpty())
        return;

    if (!walletModel->wallet().backupWallet(filename.toLocal8Bit().data())) {
        Q_EMIT message(tr("Backup Failed"), tr("There was an error trying to save the wallet data to %1.").arg(filename),
            CClientUIInterface::MSG_ERROR);
        }
    else {
        Q_EMIT message(tr("Backup Successful"), tr("The wallet data was successfully saved to %1.").arg(filename),
            CClientUIInterface::MSG_INFORMATION);
    }
}

void WalletView::changePassphrase()
{
    AskPassphraseDialog dlg(AskPassphraseDialog::ChangePass, this);
    dlg.setModel(walletModel);
    dlg.exec();
}

void WalletView::unlockWallet()
{
    if(!walletModel)
        return;
    // Unlock wallet when requested by wallet model
    if (walletModel->getEncryptionStatus() == WalletModel::Locked)
    {
        AskPassphraseDialog dlg(AskPassphraseDialog::Unlock, this);
        dlg.setModel(walletModel);
        dlg.exec();
    }
}

void WalletView::usedSendingAddresses()
{
    if(!walletModel)
        return;

    GUIUtil::bringToFront(usedSendingAddressesPage);
}

void WalletView::usedReceivingAddresses()
{
    if(!walletModel)
        return;

    GUIUtil::bringToFront(usedReceivingAddressesPage);
}

void WalletView::showProgress(const QString &title, int nProgress)
{
    if (nProgress == 0) {
        progressDialog = new QProgressDialog(title, tr("Cancel"), 0, 100);
        GUIUtil::PolishProgressDialog(progressDialog);
        progressDialog->setWindowModality(Qt::ApplicationModal);
        progressDialog->setMinimumDuration(0);
        progressDialog->setAutoClose(false);
        progressDialog->setValue(0);
    } else if (nProgress == 100) {
        if (progressDialog) {
            progressDialog->close();
            progressDialog->deleteLater();
            progressDialog = nullptr;
        }
    } else if (progressDialog) {
        if (progressDialog->wasCanceled()) {
            getWalletModel()->wallet().abortRescan();
        } else {
            progressDialog->setValue(nProgress);
        }
    }
}

void WalletView::requestedSyncWarningInfo()
{
    Q_EMIT outOfSyncWarningClicked();
}
