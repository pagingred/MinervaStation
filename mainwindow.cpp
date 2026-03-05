#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "downloadmanager.h"
#include "leaderboardheat.h"
#include "phasecolors.h"
#include "systemprofile.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QResizeEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStorageInfo>
#include <QTabWidget>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

static const char *APP_VERSION = "1.0.0";

static QString avatarCachePath()
{
    return QCoreApplication::applicationDirPath() + "/avatar.png";
}

static QString fmtBytes(qint64 aBytes)
{
    return QLocale().formattedDataSize(aBytes, 1, QLocale::DataSizeSIFormat);
}

MainWindow::~MainWindow()
{
    delete ui;
}

MainWindow::MainWindow(QWidget *aParent) : QMainWindow(aParent)
{
    mWorker = new MinervaWorker(this);
    mAvatarNam = new QNetworkAccessManager(this);
    mUiClock.start();
    ui = new Ui::MainWindow;
    ui->setupUi(this);
    SetupTables();

    connect(ui->mLoginBtn, &QPushButton::clicked, this, &MainWindow::OnLogin);
    connect(ui->mLogoutBtn, &QPushButton::clicked, this, &MainWindow::OnLogout);
    connect(ui->mStartBtn, &QPushButton::clicked, this, &MainWindow::OnStart);
    connect(ui->mPauseBtn, &QPushButton::clicked, this, &MainWindow::OnPause);
    connect(ui->mStopBtn, &QPushButton::clicked, this, &MainWindow::OnStop);
    connect(ui->mRecommendBtn, &QPushButton::clicked, this, &MainWindow::OnRecommendedSettings);
    connect(ui->browseTempBtn, &QPushButton::clicked, this, &MainWindow::OnBrowseTempDir);
    connect(ui->browseDlBtn, &QPushButton::clicked, this, &MainWindow::OnBrowseDownloadsDir);
    connect(ui->mCheckServersBtn, &QPushButton::clicked, this, &MainWindow::OnCheckServers);

    ui->mLogoutBtn->hide();
    ui->mUserAvatar->hide();
    ui->mPauseBtn->hide();
    ui->mStopBtn->hide();
    ui->mVersionLabel->setText(QString("Version: %1").arg(APP_VERSION));

    ui->mTempDir->setPlaceholderText("Default: " + QCoreApplication::applicationDirPath() + "/temp");
    ui->mDownloadsDir->setPlaceholderText("Default: " + QCoreApplication::applicationDirPath() + "/downloads");

    ui->mAria2cLabel->setText(DownloadManager::HasAria2c()
                                  ? "aria2c: found" : "aria2c: NOT found (using Qt HTTP for all downloads)");
    ui->mAria2cLabel->setStyleSheet(DownloadManager::HasAria2c() ? "color: green;" : "color: orange;");

    QString notChecked = QStringLiteral("<span style='color:#888;'>\u25CF</span> Not checked");
    ui->mStatusApiVersion->setText(notChecked);
    ui->mStatusApiJobs->setText(notChecked);
    ui->mStatusApiReport->setText(notChecked);
    ui->mStatusApiLeaderboard->setText(notChecked);
    ui->mStatusGateUpload->setText(notChecked);

    ui->mLbRefreshLabel->installEventFilter(this);

    LoadSettings();
    RefreshLoginLabel();

    connect(mWorker, &MinervaWorker::Log, this, [this](const QString &aMsg)
            {
                AppendLog(aMsg);
            });

    connect(mWorker, &MinervaWorker::Started, this, [this]()
            {
                SetRunningUi(true);
            });
    connect(mWorker, &MinervaWorker::Stopped, this, [this]()
            {
                SetRunningUi(false);
            });

    mFlushTimer = new QTimer(this);
    mFlushTimer->setSingleShot(true);
    connect(mFlushTimer, &QTimer::timeout, this, &MainWindow::FlushJobUpdates);

    connect(mWorker, &MinervaWorker::JobUpdated, this, [this](int aFileId, const JobState &aState)
            {
                if (aState.phase == JobPhase::Downloading)
                {
                    mLiveDownloadProgress[aFileId] = aState.progress;
                }
                else if (aState.phase == JobPhase::Uploading)
                {
                    mLiveUploadProgress[aFileId] = aState.progress;
                }
                bool isUploadPhase = (aState.phase == JobPhase::QueuedUpload ||
                                      aState.phase == JobPhase::Uploading ||
                                      aState.phase == JobPhase::UploadRetryWait ||
                                      aState.phase == JobPhase::Reporting);
                if (aState.phase == JobPhase::Done || aState.phase == JobPhase::Failed)
                {
                    mLiveDownloadProgress.remove(aFileId);
                    mLiveUploadProgress.remove(aFileId);
                }
                if (isUploadPhase)
                {
                    mLiveDownloadProgress.remove(aFileId);
                }

                mPendingJobStates[aFileId] = aState;
                if (!mFlushTimer->isActive())
                {
                    mFlushTimer->start(mUiUpdateIntervalMs);
                }
            });

    connect(mWorker, &MinervaWorker::JobRemoved, this, [this](int aFileId)
            {
                for (QTableWidget *table : {ui->mUploadTable, ui->mDownloadTable})
                {
                    for (int r = 0; r < table->rowCount(); ++r)
                    {
                        if (table->item(r, 0) &&
                            table->item(r, 0)->data(Qt::UserRole).toInt() == aFileId)
                        {
                            table->removeRow(r);
                            break;
                        }
                    }
                }
                mProgressBars.remove(aFileId);
                mLastUiUpdate.remove(aFileId);
                mLiveDownloadProgress.remove(aFileId);
                mLiveUploadProgress.remove(aFileId);
            });

    connect(mWorker, &MinervaWorker::StatsChanged, this,
            [this](int ok, int fail, qint64 bytesUp, qint64 bytesDown)
            {
                ui->mOkLabel->setText(QString::number(ok));
                ui->mFailLabel->setText(QString::number(fail));
                mBytesUp = bytesUp;
                mBytesDown = bytesDown;
            });

    connect(mWorker, &MinervaWorker::LoginResult, this, [this](bool ok, const QString &detail)
            {
                if (ok)
                {
                    AppendLog("Login successful!");
                    RefreshLoginLabel();
                }
                else
                {
                    AppendLog("Login failed: " + detail);
                }
            });

    std::function<void()> pushLiveConfig = [this]()
    {
        if (!mWorker->IsRunning())
        {
            return;
        }
        MinervaConfig cfg = mWorker->Config();
        cfg.dlConcurrency = ui->mDlConcurrency->value();
        cfg.ulConcurrency = ui->mUlConcurrency->value();
        cfg.batchSize = ui->mBatchSize->value();
        cfg.aria2cConnections = ui->mAria2cConns->value();
        cfg.downloadRetryDelaySec = ui->mDownloadRetryDelay->value();
        cfg.uploadRetryDelaySec = ui->mUploadRetryDelay->value();
        cfg.diskReserveBytes = static_cast<qint64>(ui->mDiskReserveMb->value()) * 1024 * 1024;
        cfg.reportRetries = ui->mReportRetries->value();
        cfg.queuePrefetch = ui->mQueuePrefetch->value();
        cfg.uploadChunkSize = static_cast<qint64>(ui->mUploadChunkSizeMb->value()) * 1024 * 1024;
        cfg.uploadStartRetries = ui->mUploadStartRetries->value();
        cfg.uploadChunkRetries = ui->mUploadChunkRetries->value();
        cfg.uploadFinishRetries = ui->mUploadFinishRetries->value();
        cfg.uploadRetryCap = static_cast<float>(ui->mUploadRetryCap->value());
        cfg.uploadChunkRetryCap = static_cast<float>(ui->mUploadChunkRetryCap->value());
        mWorker->UpdateConfig(cfg);
    };
    connect(ui->mDlConcurrency, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mUlConcurrency, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mBatchSize,     QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mAria2cConns,   QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mDownloadRetryDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mUploadRetryDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mDiskReserveMb, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mReportRetries, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mQueuePrefetch, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mUploadChunkSizeMb, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mUploadStartRetries, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mUploadChunkRetries, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mUploadFinishRetries, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mUploadRetryCap, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mUploadChunkRetryCap, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mUiUpdateInterval, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int aVal)
            {
                mUiUpdateIntervalMs = aVal;
            });

    connect(mWorker, &MinervaWorker::LeaderboardResult, this, [this](const QJsonArray &aRows, int aPage)
            {
                mLbFetchingPage = false;
                if (!mLbCooldown->isActive())
                {
                    ui->mLbRefreshLabel->setText("Refresh");
                    ui->mLbRefreshLabel->setEnabled(true);
                }
                LbAppendPageData(aRows, aPage);
            });

    connect(ui->mLbTable->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]()
            {
                if (mLbDisplaying)
                {
                    return;
                }
                int lastVisible = ui->mLbTable->rowAt(ui->mLbTable->viewport()->height() - 1);
                if (lastVisible >= 0 && mLbRealRowCount > 0 && lastVisible >= mLbRealRowCount - 15)
                {
                    LbFetchNextPageIfNeeded();
                }
            });

    connect(ui->mLbTable->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int col)
            {
                if (col == 1)
                {
                    return;
                }
                if (mLbSortCol == col)
                {
                    mLbSortAsc = !mLbSortAsc;
                }
                else
                {
                    mLbSortCol = col;
                    mLbSortAsc = (col == 0);
                }
                LbRefreshDisplay();
            });

    connect(ui->mLbSearch, &QLineEdit::textChanged, this, [this]()
            {
                LbRefreshDisplay();
            });

    mUptimeTimer = new QTimer(this);
    connect(mUptimeTimer, &QTimer::timeout, this, &MainWindow::UpdateUptime);

    mDiskTimer = new QTimer(this);
    connect(mDiskTimer, &QTimer::timeout, this, &MainWindow::UpdateDiskUsage);
    mDiskTimer->start(5000);
    UpdateDiskUsage();

    mLbTimer = new QTimer(this);
    connect(mLbTimer, &QTimer::timeout, this, &MainWindow::LbRefreshVisible);
    mLbTimer->start(15000);

    mLbCooldown = new QTimer(this);
    mLbCooldown->setInterval(1000);
    connect(mLbCooldown, &QTimer::timeout, this, [this]()
            {
                mLbCooldownSecs--;
                if (mLbCooldownSecs <= 0)
                {
                    mLbCooldown->stop();
                    ui->mLbRefreshLabel->setEnabled(true);
                    ui->mLbRefreshLabel->setText("Refresh");
                }
                else
                {
                    ui->mLbRefreshLabel->setText(QString::fromUtf8("\u27f3 %1s").arg(mLbCooldownSecs));
                }
            });

    OnRefreshLeaderboard();
}

void MainWindow::FlushJobUpdates()
{
    if (mPendingJobStates.isEmpty())
    {
        return;
    }

    QHash<int, JobState> batch;
    batch.swap(mPendingJobStates);

    QScrollBar *ulScroll = ui->mUploadTable->verticalScrollBar();
    QScrollBar *dlScroll = ui->mDownloadTable->verticalScrollBar();
    bool ulWasAtTop = (ulScroll->value() <= ulScroll->singleStep());
    bool dlWasAtTop = (dlScroll->value() <= dlScroll->singleStep());

    ui->mUploadTable->setUpdatesEnabled(false);
    ui->mDownloadTable->setUpdatesEnabled(false);

    for (QHash<int, JobState>::const_iterator it = batch.constBegin(); it != batch.constEnd(); ++it)
    {
        int aFileId = it.key();
        const JobState &aState = it.value();

        bool isUploadPhase = (aState.phase == JobPhase::QueuedUpload ||
                              aState.phase == JobPhase::Uploading ||
                              aState.phase == JobPhase::UploadRetryWait ||
                              aState.phase == JobPhase::Reporting);

        QTableWidget *targetTable = isUploadPhase ? ui->mUploadTable : ui->mDownloadTable;
        QTableWidget *otherTable = isUploadPhase ? ui->mDownloadTable : ui->mUploadTable;

        if (aState.phase == JobPhase::Done || aState.phase == JobPhase::Failed)
        {
            for (int r = 0; r < ui->mUploadTable->rowCount(); ++r)
            {
                if (ui->mUploadTable->item(r, 0) &&
                    ui->mUploadTable->item(r, 0)->data(Qt::UserRole).toInt() == aFileId)
                {
                    targetTable = ui->mUploadTable;
                    otherTable = ui->mDownloadTable;
                    break;
                }
            }
        }

        for (int r = 0; r < otherTable->rowCount(); ++r)
        {
            if (otherTable->item(r, 0) &&
                otherTable->item(r, 0)->data(Qt::UserRole).toInt() == aFileId)
            {
                mProgressBars.remove(aFileId);
                otherTable->removeRow(r);
                break;
            }
        }

        int row = -1;
        for (int r = 0; r < targetTable->rowCount(); ++r)
        {
            if (targetTable->item(r, 0) &&
                targetTable->item(r, 0)->data(Qt::UserRole).toInt() == aFileId)
            {
                row = r;
                break;
            }
        }

        int newKey = PhaseColors::SortKey(aState.phase, aState.progress, aState.total);
        if (row >= 0)
        {
            QTableWidgetItem *phaseItem = targetTable->item(row, 1);
            int oldKey = phaseItem ? phaseItem->data(Qt::UserRole).toInt() : -1;
            if (oldKey != newKey)
            {
                mProgressBars.remove(aFileId);
                targetTable->removeRow(row);
                row = -1;
            }
        }

        bool isNewRow = (row < 0);
        if (isNewRow)
        {
            row = targetTable->rowCount();
            for (int r = 0; r < targetTable->rowCount(); ++r)
            {
                QTableWidgetItem *pi = targetTable->item(r, 1);
                if (pi && pi->data(Qt::UserRole).toInt() > newKey)
                {
                    row = r;
                    break;
                }
            }
            targetTable->insertRow(row);
            QTableWidgetItem *idItem = new QTableWidgetItem(QString::number(aFileId));
            idItem->setData(Qt::UserRole, aFileId);
            targetTable->setItem(row, 0, idItem);

            QProgressBar *bar = new QProgressBar;
            bar->setMinimum(0);
            bar->setMaximum(100);
            bar->setValue(0);
            bar->setTextVisible(true);
            bar->setStyleSheet(PhaseColors::BarStyle(aState.phase));
            bar->setProperty("_phase", static_cast<int>(aState.phase));
            targetTable->setCellWidget(row, 3, bar);
            mProgressBars.insert(aFileId, bar);
        }

        QString phaseStr;
        switch (aState.phase)
        {
        case JobPhase::Queued:          phaseStr = "Queued"; break;
        case JobPhase::Downloading:     phaseStr = "Downloading"; break;
        case JobPhase::QueuedUpload:    phaseStr = "Queued"; break;
        case JobPhase::Uploading:       phaseStr = "Uploading"; break;
        case JobPhase::UploadRetryWait: phaseStr = "Retry Wait"; break;
        case JobPhase::Reporting:       phaseStr = "Reporting"; break;
        case JobPhase::Done:            phaseStr = "Done"; break;
        case JobPhase::Failed:          phaseStr = "Failed"; break;
        }
        if (aState.attempt > 1)
        {
            phaseStr += QString(" (%1)").arg(aState.attempt);
        }

        {
            QString statusText;
            if (aState.phase == JobPhase::Failed || aState.phase == JobPhase::UploadRetryWait || !aState.error.isEmpty())
            {
                statusText = aState.error;
            }

            std::function<void(int, const QString &)> setCell = [&](int col, const QString &text)
            {
                if (!targetTable->item(row, col))
                {
                    targetTable->setItem(row, col, new QTableWidgetItem());
                }
                QTableWidgetItem *item = targetTable->item(row, col);
                if (item->text() != text)
                {
                    item->setText(text);
                }
            };
            setCell(1, phaseStr);
            if (isNewRow)
            {
                setCell(2, MinervaWorker::PrettyPath(aState.info.destPath));
            }
            setCell(4, statusText);

            if (targetTable->item(row, 1))
            {
                targetTable->item(row, 1)->setData(Qt::UserRole, newKey);
            }

            if (isNewRow)
            {
                QColor fg = PhaseColors::Color(aState.phase);
                QColor bg = PhaseColors::Background(aState.phase);
                for (int c = 0; c < targetTable->columnCount(); ++c)
                {
                    if (targetTable->item(row, c))
                    {
                        targetTable->item(row, c)->setForeground(fg);
                        targetTable->item(row, c)->setBackground(bg);
                    }
                }
            }
        }

        if (mProgressBars.contains(aFileId))
        {
            QProgressBar *bar = mProgressBars[aFileId];

            int cachedPhase = bar->property("_phase").toInt();
            if (cachedPhase != static_cast<int>(aState.phase))
            {
                bar->setStyleSheet(PhaseColors::BarStyle(aState.phase));
                bar->setProperty("_phase", static_cast<int>(aState.phase));
            }

            if (aState.phase == JobPhase::Done)
            {
                bar->setMaximum(100);
                bar->setValue(100);
                bar->setFormat("Done");
            }
            else if (aState.total > 0 && aState.progress >= 0)
            {
                qint64 effectiveTotal = std::max(aState.total, aState.progress);
                bar->setMaximum(100);
                int pct = effectiveTotal > 0
                              ? static_cast<int>(aState.progress * 100 / effectiveTotal) : 0;
                bar->setValue(std::min(pct, 100));
                if (aState.phase == JobPhase::Uploading && pct >= 100)
                {
                    bar->setFormat("Finalizing...");
                }
                else
                {
                    bar->setFormat(QString("%1 / %2  (%p%)")
                                       .arg(fmtBytes(aState.progress))
                                       .arg(fmtBytes(effectiveTotal)));
                }
            }
            else if (aState.progress > 0)
            {
                bar->setMaximum(0);
                bar->setFormat(fmtBytes(aState.progress));
            }
            else if (aState.phase == JobPhase::UploadRetryWait ||
                     aState.phase == JobPhase::Queued ||
                     aState.phase == JobPhase::QueuedUpload)
            {
                bar->setMaximum(100);
                bar->setValue(0);
                bar->setFormat(phaseStr);
            }
            else
            {
                bar->setMaximum(0);
                bar->setFormat("");
            }
        }

        {
            int cachedAction = 0;
            QWidget *existing = targetTable->cellWidget(row, 5);
            if (existing)
            {
                cachedAction = existing->property("_action").toInt();
            }

            int wantedAction = 0;
            if (aState.phase == JobPhase::UploadRetryWait)
            {
                wantedAction = 1;
            }
            else if (aState.phase == JobPhase::Uploading ||
                     aState.phase == JobPhase::Downloading)
            {
                wantedAction = 2;
            }

            if (cachedAction != wantedAction)
            {
                targetTable->removeCellWidget(row, 5);
                if (wantedAction == 1)
                {
                    QPushButton *retryBtn = new QPushButton(QString::fromUtf8("\u21bb"));
                    retryBtn->setFixedSize(28, 22);
                    retryBtn->setToolTip("Retry now");
                    retryBtn->setProperty("_action", 1);
                    retryBtn->setStyleSheet(
                        "QPushButton { border: 1px solid #555; border-radius: 3px; "
                        "background: #383838; color: #f39c12; font-size: 14px; padding: 0; }"
                        "QPushButton:hover { background: #4a4a4a; }");
                    connect(retryBtn, &QPushButton::clicked, this, [this, aFileId]()
                            {
                                mWorker->ForceRetryUpload(aFileId);
                            });
                    targetTable->setCellWidget(row, 5, retryBtn);
                }
                else if (wantedAction == 2)
                {
                    QPushButton *cancelBtn = new QPushButton(QString::fromUtf8("\u2715"));
                    cancelBtn->setFixedSize(28, 22);
                    cancelBtn->setToolTip("Cancel");
                    cancelBtn->setProperty("_action", 2);
                    cancelBtn->setStyleSheet(
                        "QPushButton { border: 1px solid #555; border-radius: 3px; "
                        "background: #383838; color: #c0392b; font-size: 12px; padding: 0; }"
                        "QPushButton:hover { background: #4a4a4a; }");
                    targetTable->setCellWidget(row, 5, cancelBtn);
                }
            }
        }

        if (aState.phase == JobPhase::Done || aState.phase == JobPhase::Failed)
        {
            mPendingRemovals[aFileId] = qMakePair(QDateTime::currentMSecsSinceEpoch(), aState);
        }
    }

    ui->mUploadTable->setUpdatesEnabled(true);
    ui->mDownloadTable->setUpdatesEnabled(true);

    if (ulWasAtTop)
    {
        ulScroll->setValue(0);
    }
    if (dlWasAtTop)
    {
        dlScroll->setValue(0);
    }
}

void MainWindow::SetupTables()
{
    std::function<void(QTableWidget *)> setupQueueTable = [](QTableWidget *table)
    {
        table->verticalHeader()->setVisible(false);
        QHeaderView *hdr = table->horizontalHeader();
        hdr->setStretchLastSection(false);
        hdr->setSectionResizeMode(0, QHeaderView::Fixed);
        hdr->setSectionResizeMode(1, QHeaderView::Fixed);
        hdr->setSectionResizeMode(2, QHeaderView::Stretch);
        hdr->setSectionResizeMode(3, QHeaderView::Fixed);
        hdr->setSectionResizeMode(4, QHeaderView::Fixed);
        hdr->setSectionResizeMode(5, QHeaderView::Fixed);
        table->setColumnWidth(0, 50);
        table->setColumnWidth(1, 90);
        table->setColumnWidth(3, 250);
        table->setColumnWidth(4, 170);
        table->setColumnWidth(5, 36);
    };
    setupQueueTable(ui->mUploadTable);
    setupQueueTable(ui->mDownloadTable);

    {
        ui->mFinishedTable->verticalHeader()->setVisible(false);
        QHeaderView *hdr = ui->mFinishedTable->horizontalHeader();
        hdr->setStretchLastSection(false);
        hdr->setSectionResizeMode(0, QHeaderView::Fixed);
        hdr->setSectionResizeMode(1, QHeaderView::Stretch);
        hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        hdr->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        hdr->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        ui->mFinishedTable->setColumnWidth(0, 50);
    }

    {
        ui->mLbTable->verticalHeader()->setVisible(false);
        ui->mLbTable->setSortingEnabled(false);
        ui->mLbTable->horizontalHeader()->setSectionsClickable(true);
        ui->mLbTable->horizontalHeader()->setHighlightSections(false);
        QHeaderView *lbHeader = ui->mLbTable->horizontalHeader();
        lbHeader->setStretchLastSection(false);
        lbHeader->setSectionResizeMode(0, QHeaderView::Fixed);
        lbHeader->setSectionResizeMode(1, QHeaderView::Fixed);
        lbHeader->setSectionResizeMode(2, QHeaderView::Stretch);
        lbHeader->setSectionResizeMode(3, QHeaderView::Fixed);
        lbHeader->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        ui->mLbTable->setColumnWidth(0, 50);
        ui->mLbTable->setColumnWidth(1, 32);
        ui->mLbTable->setColumnWidth(3, 140);
    }
}

void MainWindow::LbAppendPageData(const QJsonArray &aRows, int aPage)
{
    QVector<LbEntry> newEntries;
    for (int i = 0; i < aRows.size(); ++i)
    {
        QJsonObject obj = aRows[i].toObject();
        LbEntry e;
        e.rank = obj.value("rank").toInt(i + 1);
        e.username = obj.value("discord_username").toString();
        e.bytes = obj.value("total_bytes").toInteger(0);
        e.files = obj.value("total_files").toInt();
        e.avatarUrl = obj.value("avatar_url").toString();
        newEntries.append(e);
    }

    if (aRows.size() < 50)
    {
        mLbAllPagesLoaded = true;
    }

    if (aPage > mLbHighestPage)
    {
        mLbHighestPage = aPage;
    }

    QMap<QString, LbEntry> merged;
    for (const LbEntry &e : mLbAllRows)
    {
        merged[e.username] = e;
    }
    for (const LbEntry &e : newEntries)
    {
        if (!e.username.isEmpty())
        {
            merged[e.username] = e;
        }
    }

    mLbAllRows.clear();
    mLbAllRows.reserve(merged.size());
    for (QMap<QString, LbEntry>::const_iterator it = merged.constBegin(); it != merged.constEnd(); ++it)
    {
        mLbAllRows.append(it.value());
    }

    mLbPageFetchTimes[aPage] = QDateTime::currentMSecsSinceEpoch();

    if (!mLoggedInUsername.isEmpty())
    {
        bool found = false;
        for (const LbEntry &e : mLbAllRows)
        {
            if (e.username.compare(mLoggedInUsername, Qt::CaseInsensitive) == 0)
            {
                ui->mRankLabel->setText(QString("#%1").arg(e.rank));
                ui->mVerifiedLabel->setText(fmtBytes(e.bytes));
                found = true;
                break;
            }
        }
        if (!found)
        {
            ui->mRankLabel->setText("--");
            ui->mVerifiedLabel->setText("--");
        }
    }

    LbRefreshDisplay();
}

void MainWindow::LbRefreshDisplay()
{
    mLbDisplaying = true;

    int savedScroll = ui->mLbTable->verticalScrollBar()->value();

    QString filter = ui->mLbSearch->text().trimmed();
    QVector<const LbEntry*> filtered;
    for (const LbEntry &e : mLbAllRows)
    {
        if (!filter.isEmpty() && !e.username.contains(filter, Qt::CaseInsensitive))
        {
            continue;
        }
        filtered.append(&e);
    }

    int sortCol = mLbSortCol;
    bool asc = mLbSortAsc;
    std::sort(filtered.begin(), filtered.end(), [sortCol, asc](const LbEntry *a, const LbEntry *b)
              {
                  int cmp = 0;
                  switch (sortCol)
                  {
                  case 0: cmp = (a->rank < b->rank) ? -1 : (a->rank > b->rank ? 1 : 0); break;
                  case 2: cmp = QString::compare(a->username, b->username, Qt::CaseInsensitive); break;
                  case 3: cmp = (a->bytes < b->bytes) ? -1 : (a->bytes > b->bytes ? 1 : 0); break;
                  case 4: cmp = (a->files < b->files) ? -1 : (a->files > b->files ? 1 : 0); break;
                  default: cmp = (a->rank < b->rank) ? -1 : 1; break;
                  }
                  return asc ? (cmp < 0) : (cmp > 0);
              });

    for (int c = 0; c < 5; ++c)
    {
        QString base;
        switch (c)
        {
        case 0: base = "Rank"; break;
        case 1: base = ""; break;
        case 2: base = "Username"; break;
        case 3: base = "Contributed"; break;
        case 4: base = "Files"; break;
        }
        if (c == mLbSortCol && c != 1)
        {
            base += mLbSortAsc ? QString::fromUtf8(" \u25b2") : QString::fromUtf8(" \u25bc");
        }
        if (QTableWidgetItem *item = ui->mLbTable->horizontalHeaderItem(c))
        {
            item->setText(base);
        }
    }

    ui->mLbTable->setRowCount(0);
    for (int i = 0; i < filtered.size(); ++i)
    {
        const LbEntry &lr = *filtered[i];
        int row = ui->mLbTable->rowCount();
        ui->mLbTable->insertRow(row);
        ui->mLbTable->setRowHeight(row, 32);

        QTableWidgetItem *rankItem = new QTableWidgetItem();
        rankItem->setData(Qt::DisplayRole, lr.rank);
        ui->mLbTable->setItem(row, 0, rankItem);

        QTableWidgetItem *avatarItem = new QTableWidgetItem();
        ui->mLbTable->setItem(row, 1, avatarItem);

        ui->mLbTable->setItem(row, 2, new QTableWidgetItem(lr.username));
        ui->mLbTable->setItem(row, 3, new QTableWidgetItem(fmtBytes(lr.bytes)));

        QTableWidgetItem *filesItem = new QTableWidgetItem();
        filesItem->setData(Qt::DisplayRole, lr.files);
        ui->mLbTable->setItem(row, 4, filesItem);

        double t = LeaderboardHeat::BytesToT(lr.bytes);
        QColor bg = LeaderboardHeat::ColorForT(t);
        QColor fg = LeaderboardHeat::FgForT(t);
        for (int c = 0; c < ui->mLbTable->columnCount(); ++c)
        {
            if (QTableWidgetItem *item = ui->mLbTable->item(row, c))
            {
                item->setBackground(bg);
                item->setForeground(fg);
            }
        }

        if (lr.rank <= 3)
        {
            QFont bold = ui->mLbTable->font();
            bold.setBold(true);
            ui->mLbTable->item(row, 0)->setFont(bold);
            ui->mLbTable->item(row, 2)->setFont(bold);
            QColor medal;
            if (lr.rank == 1)
            {
                medal = QColor("#d4a017");
            }
            else if (lr.rank == 2)
            {
                medal = QColor("#a0a0a8");
            }
            else
            {
                medal = QColor("#cd7f32");
            }
            ui->mLbTable->item(row, 0)->setForeground(medal);
            ui->mLbTable->item(row, 2)->setForeground(medal);
        }

        if (!lr.avatarUrl.isEmpty())
        {
            if (mAvatarCache.contains(lr.avatarUrl))
            {
                avatarItem->setIcon(QIcon(mAvatarCache[lr.avatarUrl]));
            }
            else
            {
                QNetworkRequest req{QUrl(lr.avatarUrl)};
                req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::NoLessSafeRedirectPolicy);
                QNetworkReply *reply = mAvatarNam->get(req);
                QString aUrl = lr.avatarUrl;
                connect(reply, &QNetworkReply::finished, this, [this, reply, aUrl, row]()
                        {
                            reply->deleteLater();
                            if (reply->error() != QNetworkReply::NoError)
                            {
                                return;
                            }
                            QPixmap pm;
                            pm.loadFromData(reply->readAll());
                            if (pm.isNull())
                            {
                                return;
                            }
                            pm = pm.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                            mAvatarCache[aUrl] = pm;
                            if (row < ui->mLbTable->rowCount() && ui->mLbTable->item(row, 1))
                            {
                                ui->mLbTable->item(row, 1)->setIcon(QIcon(pm));
                            }
                        });
            }
        }
    }

    mLbRealRowCount = ui->mLbTable->rowCount();

    if (!mLbAllPagesLoaded && filter.isEmpty())
    {
        int placeholders = 150;
        for (int p = 0; p < placeholders; ++p)
        {
            int row = ui->mLbTable->rowCount();
            ui->mLbTable->insertRow(row);
            ui->mLbTable->setRowHeight(row, 32);
        }
    }

    if (filter.isEmpty())
    {
        ui->mLbCountLabel->setText(QString("%1 users loaded").arg(mLbAllRows.size()));
    }
    else
    {
        ui->mLbCountLabel->setText(QString("%1 of %2 users").arg(filtered.size()).arg(mLbAllRows.size()));
    }

    ui->mLbTable->verticalScrollBar()->setValue(savedScroll);

    mLbDisplaying = false;
}

void MainWindow::LbFetchNextPageIfNeeded()
{
    if (mLbFetchingPage || mLbAllPagesLoaded)
    {
        return;
    }
    int nextPage = mLbHighestPage + 1;
    if (nextPage < 2)
    {
        nextPage = 2;
    }
    LbFetchPage(nextPage);
}

void MainWindow::LbFetchPage(int aPage)
{
    if (mLbFetchingPage)
    {
        return;
    }
    mLbFetchingPage = true;
    mWorker->FetchLeaderboard(ui->mServerUrl->text(), MinervaWorker::LoadToken(), aPage);
}

void MainWindow::LbRefreshVisible()
{
    if (mLbAllRows.isEmpty() || mLbFetchingPage)
    {
        return;
    }

    if (ui->mTabs->currentWidget() != ui->mLbTable->parentWidget())
    {
        return;
    }

    int firstRow = ui->mLbTable->rowAt(0);
    int lastRow = ui->mLbTable->rowAt(ui->mLbTable->viewport()->height() - 1);
    if (firstRow < 0)
    {
        firstRow = 0;
    }
    if (lastRow < 0 || lastRow >= mLbRealRowCount)
    {
        lastRow = mLbRealRowCount - 1;
    }
    if (lastRow < 0)
    {
        return;
    }

    QSet<int> pages;
    for (int r = firstRow; r <= lastRow; ++r)
    {
        QTableWidgetItem *item = ui->mLbTable->item(r, 0);
        if (!item)
        {
            continue;
        }
        int rank = item->data(Qt::DisplayRole).toInt();
        if (rank > 0)
        {
            pages.insert((rank - 1) / 50 + 1);
        }
    }

    if (pages.isEmpty())
    {
        return;
    }

    QList<int> sortedPages = pages.values();
    std::sort(sortedPages.begin(), sortedPages.end());
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int page : sortedPages)
    {
        qint64 elapsed = now - mLbPageFetchTimes.value(page, 0);
        if (elapsed >= 15000)
        {
            LbFetchPage(page);
            return;
        }
    }
}

void MainWindow::AddFinishedRow(int aFileId, const JobState &aState)
{
    ui->mFinishedTable->insertRow(0);
    ui->mFinishedTable->setItem(0, 0, new QTableWidgetItem(QString::number(aFileId)));
    ui->mFinishedTable->setItem(0, 1, new QTableWidgetItem(MinervaWorker::PrettyPath(aState.info.destPath)));

    qint64 sizeBytes = aState.info.size > 0 ? aState.info.size : aState.total;
    QString sizeStr = sizeBytes > 0 ? fmtBytes(sizeBytes) : QString();
    ui->mFinishedTable->setItem(0, 2, new QTableWidgetItem(sizeStr));
    ui->mFinishedTable->item(0, 2)->setData(Qt::UserRole, sizeBytes);

    QTableWidgetItem *resultItem = new QTableWidgetItem;
    bool ok = (aState.phase == JobPhase::Done);
    if (ok)
    {
        resultItem->setText("Completed");
        resultItem->setForeground(QColor("#27ae60"));
        mFinishedDoneCount++;
    }
    else
    {
        resultItem->setText("Failed: " + aState.error.left(80));
        resultItem->setForeground(QColor("#c0392b"));
        mFinishedFailCount++;
    }
    ui->mFinishedTable->setItem(0, 3, resultItem);

    QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    ui->mFinishedTable->setItem(0, 4, new QTableWidgetItem(timeStr));

    while (ui->mFinishedTable->rowCount() > 50000)
    {
        ui->mFinishedTable->removeRow(ui->mFinishedTable->rowCount() - 1);
    }

    ui->mFinishedLabel->setText(QString("Completed / Failed History (%1 done, %2 failed)")
                                .arg(mFinishedDoneCount).arg(mFinishedFailCount));
}

void MainWindow::OnStart()
{
    bool resuming = mPaused;

    QString token = MinervaWorker::LoadToken();
    if (token.isEmpty())
    {
        QMessageBox::warning(this, "Not Logged In", "You need to log in with Discord first.");
        return;
    }

    SaveSettings();
    MinervaConfig cfg;
    cfg.serverUrl = ui->mServerUrl->text();
    cfg.uploadUrl = ui->mUploadUrl->text();
    cfg.token = token;
    cfg.dlConcurrency = ui->mDlConcurrency->value();
    cfg.ulConcurrency = ui->mUlConcurrency->value();
    cfg.batchSize = ui->mBatchSize->value();
    cfg.aria2cConnections = ui->mAria2cConns->value();
    cfg.tempDir = ui->mTempDir->text();
    cfg.downloadsDir = ui->mDownloadsDir->text();
    cfg.keepFiles = ui->mKeepFiles->isChecked();
    cfg.downloadRetryDelaySec = ui->mDownloadRetryDelay->value();
    cfg.uploadRetryDelaySec = ui->mUploadRetryDelay->value();
    cfg.diskReserveBytes = static_cast<qint64>(ui->mDiskReserveMb->value()) * 1024 * 1024;
    cfg.reportRetries = ui->mReportRetries->value();
    cfg.queuePrefetch = ui->mQueuePrefetch->value();
    cfg.uploadChunkSize = static_cast<qint64>(ui->mUploadChunkSizeMb->value()) * 1024 * 1024;
    cfg.uploadStartRetries = ui->mUploadStartRetries->value();
    cfg.uploadChunkRetries = ui->mUploadChunkRetries->value();
    cfg.uploadFinishRetries = ui->mUploadFinishRetries->value();
    cfg.uploadRetryCap = static_cast<float>(ui->mUploadRetryCap->value());
    cfg.uploadChunkRetryCap = static_cast<float>(ui->mUploadChunkRetryCap->value());

    ui->mUploadTable->setRowCount(0);
    ui->mDownloadTable->setRowCount(0);
    mProgressBars.clear();
    ui->mUploadQueueLabel->setText("Upload Queue (0)");
    ui->mDownloadQueueLabel->setText("Download Queue (0)");
    mLiveDownloadProgress.clear();
    mLiveUploadProgress.clear();
    mPendingRemovals.clear();

    if (!resuming)
    {
        mStartTime = QDateTime::currentDateTime();
        mBytesDown = 0;
        mBytesUp = 0;
        mPrevDlTotal = 0;
        mPrevUlTotal = 0;
        mSmoothDlSpeed = 0.0;
        mSmoothUlSpeed = 0.0;
        ui->mDlSpeedLabel->setText("--");
        ui->mUlSpeedLabel->setText("--");
        ui->mFinishedTable->setRowCount(0);
        mFinishedDoneCount = 0;
        mFinishedFailCount = 0;
        ui->mFinishedLabel->setText("Completed / Failed History");
        ui->mLogView->clear();
    }

    mPaused = false;
    mUptimeTimer->start(1000);
    mWorker->Start(cfg);
}

void MainWindow::OnPause()
{
    if (!mWorker->IsRunning())
    {
        return;
    }
    mPaused = true;
    mWorker->Stop();
    AppendLog("Worker paused.");
}

void MainWindow::OnStop()
{
    if (mWorker->IsRunning())
    {
        mWorker->Stop();
    }

    mPaused = false;

    ui->mUploadTable->setRowCount(0);
    ui->mDownloadTable->setRowCount(0);
    mProgressBars.clear();
    ui->mUploadQueueLabel->setText("Upload Queue (0)");
    ui->mDownloadQueueLabel->setText("Download Queue (0)");

    AppendLog("Worker stopped.");
}

void MainWindow::OnLogin()
{
    QString token = MinervaWorker::LoadToken();
    if (!token.isEmpty())
    {
        QMessageBox::information(this, "Already Logged In",
                                 "You are already logged in. Logout first to switch accounts.");
        return;
    }
    mWorker->DoLogin(ui->mServerUrl->text());
}

void MainWindow::OnLogout()
{
    if (mWorker->IsRunning())
    {
        QMessageBox::warning(this, "Worker Running", "Stop the worker before logging out.");
        return;
    }
    QFile::remove(MinervaWorker::TokenPathLocal());
    QFile::remove(avatarCachePath());
    mLoggedInUsername.clear();
    ui->mUserAvatar->hide();
    ui->mRankLabel->setText("--");
    ui->mVerifiedLabel->setText("--");
    RefreshLoginLabel();
    AppendLog("Logged out. Token and avatar cache removed.");
}

void MainWindow::OnBrowseTempDir()
{
    QString dir = QFileDialog::getExistingDirectory(this, "Select Temp Directory", ui->mTempDir->text());
    if (!dir.isEmpty())
    {
        ui->mTempDir->setText(dir);
    }
}

void MainWindow::OnBrowseDownloadsDir()
{
    QString dir = QFileDialog::getExistingDirectory(this, "Select Downloads Directory", ui->mDownloadsDir->text());
    if (!dir.isEmpty())
    {
        ui->mDownloadsDir->setText(dir);
    }
}

void MainWindow::OnCheckServers()
{
    ui->mCheckServersBtn->setEnabled(false);
    ui->mCheckServersBtn->setText("Checking...");

    std::function<void(QLabel *, int, const QString &)> setStatus = [](QLabel *lbl, int code, const QString &err)
    {
        QString dot, text;
        if (code >= 200 && code < 300)
        {
            dot = "<span style='color:#27ae60;'>\u25CF</span>";
            text = QString(" %1 OK").arg(code);
        }
        else if (code == 401)
        {
            dot = "<span style='color:#f39c12;'>\u25CF</span>";
            text = QString(" %1 Unauthorized (need login)").arg(code);
        }
        else if (code == 405)
        {
            dot = "<span style='color:#27ae60;'>\u25CF</span>";
            text = QString(" %1 Reachable").arg(code);
        }
        else if (code == 426)
        {
            dot = "<span style='color:#f39c12;'>\u25CF</span>";
            text = QString(" %1 Reachable (upgrade required)").arg(code);
        }
        else if (code > 0)
        {
            dot = "<span style='color:#c0392b;'>\u25CF</span>";
            text = QString(" %1 %2").arg(code).arg(err.left(40));
        }
        else
        {
            dot = "<span style='color:#c0392b;'>\u25CF</span>";
            text = " " + err.left(50);
        }
        lbl->setText(dot + text);
    };

    QString apiBase = ui->mServerUrl->text().trimmed();
    QString gateBase = ui->mUploadUrl->text().trimmed();
    QString token = MinervaWorker::LoadToken();

    QNetworkAccessManager *nam = mAvatarNam;

    struct Endpoint { QString url; QLabel *label; bool needsAuth; bool useHead; };
    QVector<Endpoint> endpoints = {
                                   { apiBase + "/worker/version",  ui->mStatusApiVersion,     false, false },
                                   { apiBase + "/api/jobs?count=0", ui->mStatusApiJobs,       true,  false },
                                   { apiBase + "/api/jobs/report", ui->mStatusApiReport,      true,  true  },
                                   { apiBase + "/api/leaderboard", ui->mStatusApiLeaderboard, false, false },
                                   { gateBase + "/api/upload/0/start", ui->mStatusGateUpload, true,  true  },
                                   };

    int *pending = new int(endpoints.size());
    std::function<void()> checkDone = [this, pending]()
    {
        (*pending)--;
        if (*pending <= 0)
        {
            delete pending;
            ui->mCheckServersBtn->setEnabled(true);
            ui->mCheckServersBtn->setText("Check Servers");
        }
    };

    for (const Endpoint &ep : endpoints)
    {
        ep.label->setText(QStringLiteral("<span style='color:#888;'>\u25CF</span> Checking..."));

        QNetworkRequest req(QUrl(ep.url));
        req.setTransferTimeout(10000);
        req.setRawHeader("X-Minerva-Worker-Version", APP_VERSION);
        if (ep.needsAuth && !token.isEmpty())
        {
            req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
        }

        QNetworkReply *reply = ep.useHead ? nam->head(req) : nam->get(req);
        QLabel *lbl = ep.label;
        connect(reply, &QNetworkReply::finished, this, [reply, lbl, setStatus, checkDone]()
                {
                    reply->deleteLater();
                    int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    QString err = reply->errorString();
                    if (code == 0 && reply->error() != QNetworkReply::NoError)
                    {
                        err = reply->errorString();
                    }
                    setStatus(lbl, code, err);
                    checkDone();
                });
    }
}

void MainWindow::OnRecommendedSettings()
{
    if (!mProfiler)
    {
        mProfiler = new SystemProfiler(this);
    }

    QString tempPath = ui->mTempDir->text().trimmed();
    if (tempPath.isEmpty())
    {
        tempPath = QCoreApplication::applicationDirPath() + "/temp";
    }
    SystemInfo sysInfo = mProfiler->DetectSystemInfo(tempPath);

    QDialog *dlg = new QDialog(this);
    dlg->setWindowTitle("Recommended Settings");
    dlg->setMinimumSize(480, 420);
    QVBoxLayout *layout = new QVBoxLayout(dlg);

    QGroupBox *sysBox = new QGroupBox("System Info");
    QFormLayout *sysForm = new QFormLayout(sysBox);
    sysForm->addRow("CPU Cores:", new QLabel(QString::number(sysInfo.cpuCores)));
    sysForm->addRow("Architecture:", new QLabel(sysInfo.cpuArch));
    sysForm->addRow("OS:", new QLabel(sysInfo.osName));
    if (sysInfo.totalRamBytes > 0)
    {
        sysForm->addRow("RAM:", new QLabel(QString("%1 total, %2 available")
                                               .arg(fmtBytes(sysInfo.totalRamBytes))
                                               .arg(fmtBytes(sysInfo.availRamBytes))));
    }
    sysForm->addRow("Disk Type:", new QLabel(sysInfo.diskType));
    if (sysInfo.diskTotalBytes > 0)
    {
        sysForm->addRow("Disk Space:", new QLabel(QString("%1 free of %2")
                                                      .arg(fmtBytes(sysInfo.diskFreeBytes))
                                                      .arg(fmtBytes(sysInfo.diskTotalBytes))));
    }
    layout->addWidget(sysBox);

    QGroupBox *speedBox = new QGroupBox("Network Speed Test");
    QVBoxLayout *speedLayout = new QVBoxLayout(speedBox);

    QProgressBar *speedProgress = new QProgressBar;
    speedProgress->setRange(0, 100);
    speedProgress->setValue(0);
    speedLayout->addWidget(speedProgress);

    QLabel *speedStatusLabel = new QLabel("Click 'Run Speed Test' to begin");
    speedLayout->addWidget(speedStatusLabel);

    QHBoxLayout *speedRow = new QHBoxLayout;
    QLabel *dlSpeedVal = new QLabel("--");
    dlSpeedVal->setStyleSheet("font-weight: bold;");
    QLabel *ulSpeedVal = new QLabel("--");
    ulSpeedVal->setStyleSheet("font-weight: bold;");
    speedRow->addWidget(new QLabel("Download:"));
    speedRow->addWidget(dlSpeedVal);
    speedRow->addSpacing(20);
    speedRow->addWidget(new QLabel("Upload:"));
    speedRow->addWidget(ulSpeedVal);
    speedRow->addStretch();
    speedLayout->addLayout(speedRow);

    QPushButton *runSpeedBtn = new QPushButton("Run Speed Test");
    speedLayout->addWidget(runSpeedBtn);
    layout->addWidget(speedBox);

    QGroupBox *recBox = new QGroupBox("Recommended Values");
    QFormLayout *recForm = new QFormLayout(recBox);
    QLabel *recConcurrency = new QLabel("--");
    recConcurrency->setStyleSheet("font-weight: bold;");
    QLabel *recBatch = new QLabel("--");
    recBatch->setStyleSheet("font-weight: bold;");
    QLabel *recAria2c = new QLabel("--");
    recAria2c->setStyleSheet("font-weight: bold;");
    recForm->addRow("Concurrency:", recConcurrency);
    recForm->addRow("Batch Size:", recBatch);
    recForm->addRow("aria2c Connections:", recAria2c);
    layout->addWidget(recBox);

    QHBoxLayout *btnRow = new QHBoxLayout;
    QPushButton *applyBtn = new QPushButton("Apply");
    applyBtn->setEnabled(false);
    QPushButton *closeBtn = new QPushButton("Close");
    btnRow->addStretch();
    btnRow->addWidget(applyBtn);
    btnRow->addWidget(closeBtn);
    layout->addLayout(btnRow);

    RecommendedSettings *rec = new RecommendedSettings;

    connect(runSpeedBtn, &QPushButton::clicked, dlg, [this, runSpeedBtn, speedProgress, speedStatusLabel]()
            {
                runSpeedBtn->setEnabled(false);
                speedProgress->setValue(0);
                speedStatusLabel->setText("Running...");
                mProfiler->RunSpeedTest();
            });

    connect(mProfiler, &SystemProfiler::SpeedTestProgress, dlg,
            [speedProgress, speedStatusLabel](const QString &phase, int pct)
            {
                speedProgress->setValue(pct);
                speedStatusLabel->setText(phase);
            });

    connect(mProfiler, &SystemProfiler::SpeedTestFinished, dlg,
            [sysInfo, dlSpeedVal, ulSpeedVal, recConcurrency, recBatch, recAria2c,
             runSpeedBtn, applyBtn, rec](const SpeedTestResult &result)
            {
                dlSpeedVal->setText(QString("%1 Mbps").arg(result.downloadMbps, 0, 'f', 1));
                ulSpeedVal->setText(QString("%1 Mbps").arg(result.uploadMbps, 0, 'f', 1));
                runSpeedBtn->setEnabled(true);

                *rec = SystemProfiler::Calculate(sysInfo, result);
                recConcurrency->setText(QString::number(rec->concurrency));
                recBatch->setText(QString::number(rec->batchSize));
                recAria2c->setText(QString::number(rec->aria2cConnections));
                applyBtn->setEnabled(true);
            });

    connect(applyBtn, &QPushButton::clicked, dlg, [this, dlg, rec]()
            {
                ui->mDlConcurrency->setValue(rec->concurrency);
                ui->mUlConcurrency->setValue(rec->concurrency);
                ui->mBatchSize->setValue(rec->batchSize);
                ui->mAria2cConns->setValue(rec->aria2cConnections);
                AppendLog(QString("Applied recommended settings: dl=%1, ul=%1, batch=%2, aria2c=%3")
                              .arg(rec->concurrency).arg(rec->batchSize).arg(rec->aria2cConnections));
                dlg->accept();
            });

    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::reject);

    connect(dlg, &QDialog::finished, dlg, [dlg, rec]()
            {
                delete rec;
                dlg->deleteLater();
            });

    dlg->exec();
}

void MainWindow::OnRefreshLeaderboard()
{
    mLbPageFetchTimes.clear();
    if (mLbAllRows.isEmpty())
    {
        LbFetchPage(1);
    }
    else
    {
        LbRefreshVisible();
    }
    StartRefreshCooldown();
}

void MainWindow::StartRefreshCooldown()
{
    mLbCooldownSecs = 15;
    ui->mLbRefreshLabel->setEnabled(false);
    ui->mLbRefreshLabel->setText(QString::fromUtf8("\u27f3 15s"));
    mLbCooldown->start();
}

void MainWindow::UpdateUptime()
{
    if (!mWorker->IsRunning())
    {
        return;
    }
    qint64 secs = mStartTime.secsTo(QDateTime::currentDateTime());
    int h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    ui->mUptimeLabel->setText(QString("%1:%2:%3")
                              .arg(h, 2, 10, QChar('0'))
                              .arg(m, 2, 10, QChar('0'))
                              .arg(s, 2, 10, QChar('0')));

    {
        std::function<QString(double)> fmtSpeed = [](double bps) -> QString
        {
            if (bps < 1024)
            {
                return QString::number(static_cast<int>(bps)) + " B/s";
            }
            if (bps < 1024 * 1024)
            {
                return QString::number(bps / 1024, 'f', 1) + " KB/s";
            }
            if (bps < 1024LL * 1024 * 1024)
            {
                return QString::number(bps / (1024 * 1024), 'f', 2) + " MB/s";
            }
            return QString::number(bps / (1024LL * 1024 * 1024), 'f', 2) + " GB/s";
        };

        qint64 liveDl = mBytesDown;
        for (QMap<int, qint64>::const_iterator it = mLiveDownloadProgress.constBegin(); it != mLiveDownloadProgress.constEnd(); ++it)
        {
            liveDl += it.value();
        }
        qint64 liveUl = mBytesUp;
        for (QMap<int, qint64>::const_iterator it = mLiveUploadProgress.constBegin(); it != mLiveUploadProgress.constEnd(); ++it)
        {
            liveUl += it.value();
        }

        double instDl = std::max(0.0, static_cast<double>(liveDl - mPrevDlTotal));
        double instUl = std::max(0.0, static_cast<double>(liveUl - mPrevUlTotal));
        mPrevDlTotal = liveDl;
        mPrevUlTotal = liveUl;

        constexpr double alpha = 0.3;
        mSmoothDlSpeed = alpha * instDl + (1.0 - alpha) * mSmoothDlSpeed;
        mSmoothUlSpeed = alpha * instUl + (1.0 - alpha) * mSmoothUlSpeed;

        ui->mDlSpeedLabel->setText(fmtSpeed(mSmoothDlSpeed));
        ui->mUlSpeedLabel->setText(fmtSpeed(mSmoothUlSpeed));

        ui->mDownloadedLabel->setText(fmtBytes(liveDl));
        ui->mUploadedLabel->setText(fmtBytes(liveUl));
    }

    ui->mUploadQueueLabel->setText(QString("Upload Queue (%1)").arg(ui->mUploadTable->rowCount()));
    ui->mDownloadQueueLabel->setText(QString("Download Queue (%1)").arg(ui->mDownloadTable->rowCount()));

    {
        qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        QMap<int, QPair<qint64, JobState>>::iterator it = mPendingRemovals.begin();
        while (it != mPendingRemovals.end())
        {
            if (nowMs - it.value().first < 3000)
            {
                ++it;
                continue;
            }
            int fid = it.key();
            JobState captured = it.value().second;
            mProgressBars.remove(fid);
            for (QTableWidget *table : {ui->mUploadTable, ui->mDownloadTable})
            {
                bool found = false;
                for (int r = 0; r < table->rowCount(); ++r)
                {
                    if (table->item(r, 0) &&
                        table->item(r, 0)->data(Qt::UserRole).toInt() == fid)
                    {
                        table->removeRow(r);
                        found = true;
                        break;
                    }
                }
                if (found)
                {
                    break;
                }
            }
            AddFinishedRow(fid, captured);
            it = mPendingRemovals.erase(it);
        }
    }
}

void MainWindow::UpdateDiskUsage()
{
    std::function<QString(const QString &)> diskForPath = [](const QString &path) -> QString
    {
        if (path.isEmpty())
        {
            return "N/A";
        }

        QString abs = QFileInfo(path).absoluteFilePath();

        QStorageInfo si(abs);
        if (si.isValid() && si.bytesTotal() > 0)
        {
            return QString("%1 free")
            .arg(fmtBytes(si.bytesAvailable()));
        }

        QStorageInfo best;
        for (const QStorageInfo &vol : QStorageInfo::mountedVolumes())
        {
            if (!vol.isValid() || vol.bytesTotal() <= 0)
            {
                continue;
            }
            if (abs.startsWith(vol.rootPath()) &&
                vol.rootPath().length() > best.rootPath().length())
            {
                best = vol;
            }
        }
        if (best.isValid())
        {
            return QString("%1 free")
            .arg(fmtBytes(best.bytesAvailable()));
        }
        return "N/A";
    };

    QString tempPath = ui->mTempDir->text().trimmed();
    if (tempPath.isEmpty())
    {
        tempPath = QCoreApplication::applicationDirPath() + "/temp";
    }
    ui->mTempDiskLabel->setText(diskForPath(tempPath));

    QString dlPath = ui->mDownloadsDir->text().trimmed();
    if (dlPath.isEmpty())
    {
        dlPath = QCoreApplication::applicationDirPath() + "/downloads";
    }
    ui->mDlDiskLabel->setText(diskForPath(dlPath));
}

void MainWindow::SyncQueueColumns()
{
    int tableW = ui->mUploadTable->viewport()->width();
    if (tableW <= 0)
    {
        tableW = ui->mDownloadTable->viewport()->width();
    }
    if (tableW <= 0)
    {
        return;
    }

    int remaining = tableW - 50 - 90 - 36;
    if (remaining < 300)
    {
        return;
    }

    int progressW = static_cast<int>(remaining * 0.38);
    progressW = std::max(progressW, 200);
    int statusW = std::max(remaining - progressW - static_cast<int>(remaining * 0.40), 120);

    for (QTableWidget *table : {ui->mUploadTable, ui->mDownloadTable})
    {
        table->setColumnWidth(3, progressW);
        table->setColumnWidth(4, statusW);
        table->setColumnWidth(5, 36);
    }
}

void MainWindow::resizeEvent(QResizeEvent *aEvent)
{
    QMainWindow::resizeEvent(aEvent);
    SyncQueueColumns();
}

void MainWindow::RefreshLoginLabel()
{
    QString token = MinervaWorker::LoadToken();
    if (token.isEmpty())
    {
        ui->mLoginLabel->setText("Not logged in");
        ui->mLoginBtn->show(); ui->mLogoutBtn->hide(); ui->mUserAvatar->hide();
    }
    else
    {
        ui->mLoginLabel->setText("Logged in");
        ui->mLoginBtn->hide(); ui->mLogoutBtn->show();
        FetchUserProfile(token);
    }
}

void MainWindow::FetchUserProfile(const QString &aToken)
{
    QStringList parts = aToken.split('.');
    if (parts.size() < 2)
    {
        AppendLog("[JWT] Token is not a valid JWT (no dots)");
        return;
    }

    QByteArray payload = parts[1].toUtf8();
    payload.replace('-', '+');
    payload.replace('_', '/');
    while (payload.size() % 4 != 0)
    {
        payload.append('=');
    }

    QByteArray decoded = QByteArray::fromBase64(payload);
    QJsonDocument doc = QJsonDocument::fromJson(decoded);
    QJsonObject obj = doc.object();

    AppendLog(QString("[JWT] Payload keys: %1").arg(QStringList(obj.keys()).join(", ")));
    AppendLog(QString("[JWT] Payload: %1").arg(QString::fromUtf8(decoded).left(500)));

    QString username;
    for (const auto &key : {"discord_username", "username", "name", "sub", "preferred_username"})
    {
        username = obj.value(key).toString();
        if (!username.isEmpty())
        {
            break;
        }
    }
    if (!username.isEmpty())
    {
        mLoggedInUsername = username;
        ui->mLoginLabel->setText(username);
    }

    QString avatarUrl;
    for (const auto &key : {"avatar_url", "discord_avatar_url", "picture"})
    {
        avatarUrl = obj.value(key).toString();
        if (!avatarUrl.isEmpty())
        {
            break;
        }
    }

    if (avatarUrl.isEmpty())
    {
        QString discordId;
        for (const auto &key : {"discord_id", "id", "user_id", "sub"})
        {
            discordId = obj.value(key).toString();
            if (discordId.isEmpty())
            {
                if (obj.value(key).isDouble())
                {
                    discordId = QString::number(obj.value(key).toInteger());
                }
            }
            if (!discordId.isEmpty())
            {
                break;
            }
        }
        QString avatarHash;
        for (const auto &key : {"avatar", "avatar_hash", "discord_avatar"})
        {
            avatarHash = obj.value(key).toString();
            if (!avatarHash.isEmpty())
            {
                break;
            }
        }
        if (!discordId.isEmpty() && !avatarHash.isEmpty())
        {
            QString ext = avatarHash.startsWith("a_") ? "gif" : "png";
            avatarUrl = QString("https://cdn.discordapp.com/avatars/%1/%2.%3")
                            .arg(discordId, avatarHash, ext);
        }
        AppendLog(QString("[JWT] Discord ID: %1, Avatar hash: %2")
                      .arg(discordId.isEmpty() ? "(empty)" : discordId)
                      .arg(avatarHash.isEmpty() ? "(empty)" : avatarHash));
    }

    AppendLog(QString("[JWT] Username: %1, Avatar: %2")
                  .arg(username.isEmpty() ? "(empty)" : username)
                  .arg(avatarUrl.isEmpty() ? "(empty)" : avatarUrl));

    std::function<void(const QByteArray &)> showAndCache = [this](const QByteArray &imgData)
    {
        QPixmap pm;
        pm.loadFromData(imgData);
        if (pm.isNull())
        {
            return;
        }
        pm = pm.scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pm.save(avatarCachePath(), "PNG");
        ui->mUserAvatar->setPixmap(pm);
        ui->mUserAvatar->show();
    };

    QPixmap cached;
    if (cached.load(avatarCachePath()))
    {
        cached = cached.scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        ui->mUserAvatar->setPixmap(cached);
        ui->mUserAvatar->show();
    }

    if (!avatarUrl.isEmpty())
    {
        QNetworkRequest avatarReq{QUrl(avatarUrl)};
        avatarReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                               QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *avatarReply = mAvatarNam->get(avatarReq);
        connect(avatarReply, &QNetworkReply::finished, this, [this, avatarReply, showAndCache]()
                {
                    avatarReply->deleteLater();
                    if (avatarReply->error() != QNetworkReply::NoError)
                    {
                        return;
                    }
                    showAndCache(avatarReply->readAll());
                });
    }
}


void MainWindow::SetRunningUi(bool aRunning)
{
    if (aRunning)
    {
        ui->mStartBtn->hide();
        ui->mPauseBtn->show();
        ui->mStopBtn->show();
    }
    else if (mPaused)
    {
        ui->mStartBtn->setText("Resume");
        ui->mStartBtn->setStyleSheet("QPushButton { font-weight: bold; padding: 6px 16px; background: #27ae60; color: white; }");
        ui->mStartBtn->show();
        ui->mPauseBtn->hide();
        ui->mStopBtn->show();
    }
    else
    {
        ui->mStartBtn->setText("Start");
        ui->mStartBtn->setStyleSheet("QPushButton { font-weight: bold; padding: 6px 16px; }");
        ui->mStartBtn->show();
        ui->mPauseBtn->hide();
        ui->mStopBtn->hide();
    }

    bool settingsEnabled = !aRunning && !mPaused;
    ui->mServerUrl->setEnabled(settingsEnabled);
    ui->mUploadUrl->setEnabled(settingsEnabled);
    ui->mTempDir->setEnabled(settingsEnabled);
    ui->mDownloadsDir->setEnabled(settingsEnabled);
    ui->mKeepFiles->setEnabled(settingsEnabled);
    ui->mRecommendBtn->setEnabled(settingsEnabled);
    ui->mDlConcurrency->setEnabled(settingsEnabled || aRunning);
    ui->mUlConcurrency->setEnabled(settingsEnabled || aRunning);
    ui->mBatchSize->setEnabled(settingsEnabled || aRunning);
    ui->mAria2cConns->setEnabled(settingsEnabled || aRunning);
    ui->mDownloadRetryDelay->setEnabled(settingsEnabled || aRunning);
    ui->mUploadRetryDelay->setEnabled(settingsEnabled || aRunning);
    ui->mDiskReserveMb->setEnabled(settingsEnabled || aRunning);
    ui->mReportRetries->setEnabled(settingsEnabled || aRunning);
    ui->mQueuePrefetch->setEnabled(settingsEnabled || aRunning);
    ui->mUploadChunkSizeMb->setEnabled(settingsEnabled || aRunning);
    ui->mUploadStartRetries->setEnabled(settingsEnabled || aRunning);
    ui->mUploadChunkRetries->setEnabled(settingsEnabled || aRunning);
    ui->mUploadFinishRetries->setEnabled(settingsEnabled || aRunning);
    ui->mUploadRetryCap->setEnabled(settingsEnabled || aRunning);
    ui->mUploadChunkRetryCap->setEnabled(settingsEnabled || aRunning);
    ui->mUiUpdateInterval->setEnabled(settingsEnabled || aRunning);
    if (!aRunning)
    {
        mUptimeTimer->stop();
    }
}

void MainWindow::LoadSettings()
{
    QSettings s(QCoreApplication::applicationDirPath() + "/MinervaStation.ini", QSettings::IniFormat);
    ui->mServerUrl->setText(s.value("serverUrl", "https://api.minerva-archive.org").toString());
    ui->mUploadUrl->setText(s.value("uploadUrl", "https://gate.minerva-archive.org").toString());
    ui->mDlConcurrency->setValue(s.value("dlConcurrency", s.value("concurrency", 5).toInt()).toInt());
    ui->mUlConcurrency->setValue(s.value("ulConcurrency", 5).toInt());
    ui->mBatchSize->setValue(s.value("batchSize", 10).toInt());
    ui->mAria2cConns->setValue(s.value("aria2cConns", 8).toInt());
    ui->mTempDir->setText(s.value("tempDir", "").toString());
    ui->mDownloadsDir->setText(s.value("downloadsDir", "").toString());
    ui->mKeepFiles->setChecked(s.value("keepFiles", false).toBool());
    ui->mDownloadRetryDelay->setValue(s.value("downloadRetryDelay", 5).toInt());
    ui->mUploadRetryDelay->setValue(s.value("uploadRetryDelay", 10).toInt());
    ui->mDiskReserveMb->setValue(s.value("diskReserveMb", 500).toInt());
    ui->mReportRetries->setValue(s.value("reportRetries", 20).toInt());
    ui->mQueuePrefetch->setValue(s.value("queuePrefetch", 2).toInt());
    ui->mUploadChunkSizeMb->setValue(s.value("uploadChunkSizeMb", 8).toInt());
    ui->mUploadStartRetries->setValue(s.value("uploadStartRetries", 12).toInt());
    ui->mUploadChunkRetries->setValue(s.value("uploadChunkRetries", 30).toInt());
    ui->mUploadFinishRetries->setValue(s.value("uploadFinishRetries", 12).toInt());
    ui->mUploadRetryCap->setValue(s.value("uploadRetryCap", 25.0).toDouble());
    ui->mUploadChunkRetryCap->setValue(s.value("uploadChunkRetryCap", 20.0).toDouble());
    ui->mUiUpdateInterval->setValue(s.value("uiUpdateInterval", 50).toInt());
    mUiUpdateIntervalMs = ui->mUiUpdateInterval->value();
}

void MainWindow::SaveSettings()
{
    QSettings s(QCoreApplication::applicationDirPath() + "/MinervaStation.ini", QSettings::IniFormat);
    s.setValue("serverUrl", ui->mServerUrl->text());
    s.setValue("uploadUrl", ui->mUploadUrl->text());
    s.setValue("dlConcurrency", ui->mDlConcurrency->value());
    s.setValue("ulConcurrency", ui->mUlConcurrency->value());
    s.setValue("batchSize", ui->mBatchSize->value());
    s.setValue("aria2cConns", ui->mAria2cConns->value());
    s.setValue("tempDir", ui->mTempDir->text());
    s.setValue("downloadsDir", ui->mDownloadsDir->text());
    s.setValue("keepFiles", ui->mKeepFiles->isChecked());
    s.setValue("downloadRetryDelay", ui->mDownloadRetryDelay->value());
    s.setValue("uploadRetryDelay", ui->mUploadRetryDelay->value());
    s.setValue("diskReserveMb", ui->mDiskReserveMb->value());
    s.setValue("reportRetries", ui->mReportRetries->value());
    s.setValue("queuePrefetch", ui->mQueuePrefetch->value());
    s.setValue("uploadChunkSizeMb", ui->mUploadChunkSizeMb->value());
    s.setValue("uploadStartRetries", ui->mUploadStartRetries->value());
    s.setValue("uploadChunkRetries", ui->mUploadChunkRetries->value());
    s.setValue("uploadFinishRetries", ui->mUploadFinishRetries->value());
    s.setValue("uploadRetryCap", ui->mUploadRetryCap->value());
    s.setValue("uploadChunkRetryCap", ui->mUploadChunkRetryCap->value());
    s.setValue("uiUpdateInterval", ui->mUiUpdateInterval->value());
}

bool MainWindow::eventFilter(QObject *aObj, QEvent *aEvent)
{
    if (aObj == ui->mLbRefreshLabel && aEvent->type() == QEvent::MouseButtonRelease)
    {
        if (ui->mLbRefreshLabel->isEnabled())
        {
            OnRefreshLeaderboard();
        }
        return true;
    }
    return QMainWindow::eventFilter(aObj, aEvent);
}

void MainWindow::AppendLog(const QString &aMsg)
{
    QString stamped = QDateTime::currentDateTime().toString("[HH:mm:ss] ") + aMsg;
    ui->mLogView->appendPlainText(stamped);
    ui->mLogView->moveCursor(QTextCursor::End);
    ui->mLogView->ensureCursorVisible();
    statusBar()->showMessage(aMsg, 10000);
}

void MainWindow::closeEvent(QCloseEvent *aEvent)
{
    SaveSettings();
    if (mWorker->IsRunning() || mPaused)
    {
        QMessageBox::StandardButton res = QMessageBox::question(this, "Worker Active",
                                         "Worker is still active. Stop and quit?");
        if (res != QMessageBox::Yes)
        {
            aEvent->ignore();
            return;
        }
        if (mWorker->IsRunning())
        {
            mWorker->Stop();
        }
        mPaused = false;
    }
    aEvent->accept();
}
