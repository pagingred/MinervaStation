#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "hyperscrapeprotocol.h"
#include "leaderboardheat.h"
#include "phasecolors.h"
#include "systemprofile.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QResizeEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
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

static QString fmtSpeed(double bps)
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
}

MainWindow::~MainWindow()
{
    mWorker->Stop();
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
    connect(ui->mCheckServersBtn, &QPushButton::clicked, this, &MainWindow::OnCheckServers);

    ui->mLogoutBtn->hide();
    ui->mUserAvatar->hide();
    ui->mPauseBtn->hide();
    ui->mStopBtn->hide();
    ui->mVersionLabel->setText(QString("Version: %1").arg(APP_VERSION));

    QString notChecked = QStringLiteral("<span style='color:#888;'>\u25CF</span> Not checked");
    ui->mStatusApiLeaderboard->setText(notChecked);
    ui->mStatusApiStats->setText(notChecked);

    ui->mLbRefreshLabel->installEventFilter(this);

    LoadSettings();
    RefreshLoginLabel();

    {
        QString token = MinervaWorker::LoadToken();
        if (!token.isEmpty())
        {
            MinervaConfig cfg;
            cfg.firehoseUrl = ui->mFirehoseUrl->text();
            cfg.serverUrl = ui->mServerUrl->text();
            cfg.token = token;
            cfg.concurrency = ui->mConcurrency->value();
            cfg.subchunkRetries = ui->mSubchunkRetries->value();
            cfg.reconnectDelaySec = ui->mReconnectDelay->value();
            mWorker->EstablishConnection(cfg);
        }
    }

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

    mLogFlushTimer = new QTimer(this);
    mLogFlushTimer->setSingleShot(true);
    connect(mLogFlushTimer, &QTimer::timeout, this, &MainWindow::FlushLogMessages);

    connect(mWorker, &MinervaWorker::JobUpdated, this, [this](int aJobIndex, const JobState &aState)
            {
                if (aState.phase == JobPhase::Downloading || aState.phase == JobPhase::Uploading)
                {
                    mLiveProgress[aJobIndex] = aState.progress;
                }
                if (aState.phase == JobPhase::Done || aState.phase == JobPhase::Failed)
                {
                    mLiveProgress.remove(aJobIndex);
                }

                mPendingJobStates[aJobIndex] = aState;
                if (!mFlushTimer->isActive())
                {
                    mFlushTimer->start(mUiUpdateIntervalMs);
                }
            });

    connect(mWorker, &MinervaWorker::JobRemoved, this, [this](int aJobIndex)
            {
                if (mChunkRowIndex.contains(aJobIndex))
                {
                    ui->mChunksTable->removeRow(mChunkRowIndex.value(aJobIndex)->row());
                    mChunkRowIndex.remove(aJobIndex);
                }
                mProgressBars.remove(aJobIndex);
                mLastUiUpdate.remove(aJobIndex);
                mLiveProgress.remove(aJobIndex);
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
                    MinervaConfig cfg;
                    cfg.firehoseUrl = ui->mFirehoseUrl->text();
                    cfg.serverUrl = ui->mServerUrl->text();
                    cfg.token = MinervaWorker::LoadToken();
                    cfg.concurrency = ui->mConcurrency->value();
                    cfg.subchunkRetries = ui->mSubchunkRetries->value();
                    cfg.reconnectDelaySec = ui->mReconnectDelay->value();
                    mWorker->EstablishConnection(cfg);
                }
                else
                {
                    AppendLog("Login failed: " + detail);
                }
            });

    connect(mWorker, &MinervaWorker::LoginPrompt, this, [this]()
            {
                bool ok = false;
                QString token = QInputDialog::getText(
                    this, "Paste Token",
                    "Paste the token shown in your browser:",
                    QLineEdit::Normal, {}, &ok);
                if (ok && !token.trimmed().isEmpty())
                {
                    mWorker->SubmitToken(token.trimmed());
                }
                else
                {
                    AppendLog("Login cancelled.");
                }
            });

    std::function<void()> pushLiveConfig = [this]()
    {
        if (!mWorker->IsRunning())
        {
            return;
        }
        MinervaConfig cfg = mWorker->Config();
        cfg.concurrency = ui->mConcurrency->value();
        cfg.subchunkRetries = ui->mSubchunkRetries->value();
        cfg.reconnectDelaySec = ui->mReconnectDelay->value();
        mWorker->UpdateConfig(cfg);
    };
    connect(ui->mConcurrency, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mSubchunkRetries, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
    connect(ui->mReconnectDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, pushLiveConfig);
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

    connect(mWorker, &MinervaWorker::NetworkStatsResult, this, [this](const QJsonObject &stats)
            {
                ui->mNetWorkersLabel->setText(QString::number(stats["active_workers"].toInt()));
                ui->mNetSpeedLabel->setText(fmtSpeed(stats["current_speed"].toDouble()));

                int completedFiles = stats["completed_files"].toInt();
                int totalFiles = stats["total_files"].toInt();
                ui->mNetFilesLabel->setText(QString("%1 / %2")
                    .arg(QLocale().toString(completedFiles), QLocale().toString(totalFiles)));

                int completedChunks = stats["completed_chunks"].toInt();
                int totalChunks = stats["total_chunks"].toInt();
                ui->mNetChunksLabel->setText(QString("%1 / %2")
                    .arg(QLocale().toString(completedChunks), QLocale().toString(totalChunks)));

                qint64 dlBytes = stats["downloaded_bytes"].toInteger();
                qint64 totalBytes = stats["total_bytes"].toInteger();
                ui->mNetDownloadedLabel->setText(QString("%1 / %2")
                    .arg(fmtBytes(dlBytes), fmtBytes(totalBytes)));

                int pending = stats["pending"].toInt();
                ui->mNetPendingLabel->setText(QLocale().toString(pending));
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

    QScrollBar *scroll = ui->mChunksTable->verticalScrollBar();
    bool wasAtTop = (scroll->value() <= scroll->singleStep());

    ui->mChunksTable->setUpdatesEnabled(false);

    for (QHash<int, JobState>::const_iterator it = batch.constBegin(); it != batch.constEnd(); ++it)
    {
        int jobIdx = it.key();
        const JobState &aState = it.value();

        if (aState.phase == JobPhase::Queued && !mChunkRowIndex.contains(jobIdx))
        {
            continue;
        }

        if (!mChunkRowIndex.contains(jobIdx))
        {
            int row = ui->mChunksTable->rowCount();
            ui->mChunksTable->insertRow(row);
            QTableWidgetItem *idItem = new QTableWidgetItem(aState.info.chunkId.left(8));
            idItem->setData(Qt::UserRole, jobIdx);
            ui->mChunksTable->setItem(row, 0, idItem);
            mChunkRowIndex.insert(jobIdx, idItem);

            QProgressBar *bar = new QProgressBar;
            bar->setMinimum(0);
            bar->setMaximum(100);
            bar->setValue(0);
            bar->setTextVisible(true);
            bar->setStyleSheet(PhaseColors::BarStyle(aState.phase));
            bar->setProperty("_phase", static_cast<int>(aState.phase));
            ui->mChunksTable->setCellWidget(row, 3, bar);
            mProgressBars.insert(jobIdx, bar);
        }

        int row = mChunkRowIndex.value(jobIdx)->row();

        QString phaseStr;
        switch (aState.phase)
        {
        case JobPhase::Queued:      phaseStr = "Queued"; break;
        case JobPhase::Downloading: phaseStr = "Downloading"; break;
        case JobPhase::Uploading:   phaseStr = "Uploading"; break;
        case JobPhase::Done:        phaseStr = "Done"; break;
        case JobPhase::Failed:      phaseStr = "Failed"; break;
        }
        if (aState.attempt > 1)
        {
            phaseStr += QString(" (%1)").arg(aState.attempt);
        }

        QString statusText;
        if (aState.phase == JobPhase::Failed || !aState.error.isEmpty())
        {
            statusText = aState.error;
        }

        std::function<void(int, const QString &)> setCell = [&](int col, const QString &text)
        {
            if (!ui->mChunksTable->item(row, col))
            {
                ui->mChunksTable->setItem(row, col, new QTableWidgetItem());
            }
            QTableWidgetItem *item = ui->mChunksTable->item(row, col);
            if (item->text() != text)
            {
                item->setText(text);
            }
        };
        setCell(1, aState.info.fileId.left(12));
        setCell(2, phaseStr);
        setCell(4, statusText);

        {
            QColor fg = PhaseColors::Color(aState.phase);
            QColor bg = PhaseColors::Background(aState.phase);
            for (int c = 0; c < ui->mChunksTable->columnCount(); ++c)
            {
                if (ui->mChunksTable->item(row, c))
                {
                    ui->mChunksTable->item(row, c)->setForeground(fg);
                    ui->mChunksTable->item(row, c)->setBackground(bg);
                }
            }
        }

        if (mProgressBars.contains(jobIdx))
        {
            QProgressBar *bar = mProgressBars[jobIdx];

            int cachedPhase = bar->property("_phase").toInt();
            if (cachedPhase != static_cast<int>(aState.phase))
            {
                bar->setStyleSheet(PhaseColors::BarStyle(aState.phase));
                bar->setProperty("_phase", static_cast<int>(aState.phase));
            }

            int wantMax = 100, wantVal = 0;
            QString wantFmt;

            if (aState.phase == JobPhase::Done)
            {
                wantVal = 100;
                wantFmt = "Done";
            }
            else if (aState.total > 0 && aState.progress >= 0)
            {
                qint64 effectiveTotal = std::max(aState.total, aState.progress);
                int pct = effectiveTotal > 0
                              ? static_cast<int>(aState.progress * 100 / effectiveTotal) : 0;
                wantVal = std::min(pct, 100);
                wantFmt = QString("%1 / %2  (%p%)")
                              .arg(fmtBytes(aState.progress))
                              .arg(fmtBytes(effectiveTotal));
            }
            else if (aState.progress > 0)
            {
                wantMax = 0;
                wantFmt = fmtBytes(aState.progress);
            }
            else
            {
                wantMax = 0;
            }

            if (bar->maximum() != wantMax)
            {
                bar->setMaximum(wantMax);
            }
            if (wantMax > 0 && bar->value() != wantVal)
            {
                bar->setValue(wantVal);
            }
            if (bar->format() != wantFmt)
            {
                bar->setFormat(wantFmt);
            }
        }

        if (aState.phase == JobPhase::Done || aState.phase == JobPhase::Failed)
        {
            mPendingRemovals[jobIdx] = qMakePair(QDateTime::currentMSecsSinceEpoch(), aState);
        }
    }

    ui->mChunksTable->setUpdatesEnabled(true);

    if (wasAtTop)
    {
        scroll->setValue(0);
    }

    {
        qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        QMap<int, QPair<qint64, JobState>>::iterator it = mPendingRemovals.begin();
        while (it != mPendingRemovals.end())
        {
            if (nowMs - it.value().first < 500)
            {
                ++it;
                continue;
            }
            int idx = it.key();
            JobState captured = it.value().second;
            mProgressBars.remove(idx);
            if (mChunkRowIndex.contains(idx))
            {
                ui->mChunksTable->removeRow(mChunkRowIndex.value(idx)->row());
                mChunkRowIndex.remove(idx);
            }
            AddFinishedRow(idx, captured);
            it = mPendingRemovals.erase(it);
        }
    }

    int active = ui->mChunksTable->rowCount();
    ui->mChunksLabel->setText(QString("Active Chunks (%1)").arg(active));
}

void MainWindow::SetupTables()
{
    {
        ui->mChunksTable->verticalHeader()->setVisible(false);
        QHeaderView *hdr = ui->mChunksTable->horizontalHeader();
        hdr->setStretchLastSection(false);
        hdr->setSectionResizeMode(0, QHeaderView::Fixed);
        hdr->setSectionResizeMode(1, QHeaderView::Fixed);
        hdr->setSectionResizeMode(2, QHeaderView::Fixed);
        hdr->setSectionResizeMode(3, QHeaderView::Stretch);
        hdr->setSectionResizeMode(4, QHeaderView::Fixed);
        ui->mChunksTable->setColumnWidth(0, 80);
        ui->mChunksTable->setColumnWidth(1, 100);
        ui->mChunksTable->setColumnWidth(2, 80);
        ui->mChunksTable->setColumnWidth(4, 170);
    }

    {
        ui->mFinishedTable->verticalHeader()->setVisible(false);
        QHeaderView *hdr = ui->mFinishedTable->horizontalHeader();
        hdr->setStretchLastSection(false);
        hdr->setSectionResizeMode(0, QHeaderView::Fixed);
        hdr->setSectionResizeMode(1, QHeaderView::Stretch);
        hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        hdr->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        hdr->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        ui->mFinishedTable->setColumnWidth(0, 80);
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
        e.rank = obj.value("rank").toInt((aPage - 1) * 50 + i + 1);
        e.username = obj.value("discord_username").toString();
        e.bytes = obj.value("downloaded_bytes").toInteger(0);
        e.chunks = obj.value("downloaded_chunks").toInt();
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
                mLbUserPage = (e.rank - 1) / 50 + 1;
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
                  case 4: cmp = (a->chunks < b->chunks) ? -1 : (a->chunks > b->chunks ? 1 : 0); break;
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
        case 4: base = "Chunks"; break;
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

        QTableWidgetItem *chunksItem = new QTableWidgetItem();
        chunksItem->setData(Qt::DisplayRole, lr.chunks);
        ui->mLbTable->setItem(row, 4, chunksItem);

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
    QString apiBase = HyperscrapeProtocol::ApiBaseFromWsUrl(ui->mFirehoseUrl->text());
    mWorker->FetchLeaderboard(apiBase, MinervaWorker::LoadToken(), aPage);
}

void MainWindow::LbRefreshVisible()
{
    if (mLbFetchingPage)
    {
        return;
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();

    bool onLbTab = (ui->mTabs->currentWidget() == ui->mLbTable->parentWidget());
    if (onLbTab && mLbRealRowCount > 0)
    {
        int firstRow = ui->mLbTable->rowAt(0);
        int lastRow = ui->mLbTable->rowAt(ui->mLbTable->viewport()->height() - 1);
        if (firstRow < 0) firstRow = 0;
        if (lastRow < 0 || lastRow >= mLbRealRowCount) lastRow = mLbRealRowCount - 1;

        if (lastRow >= 0)
        {
            QSet<int> vis;
            for (int r = firstRow; r <= lastRow; ++r)
            {
                QTableWidgetItem *item = ui->mLbTable->item(r, 0);
                if (item)
                {
                    int rank = item->data(Qt::DisplayRole).toInt();
                    if (rank > 0) vis.insert((rank - 1) / 50 + 1);
                }
            }
            if (!vis.isEmpty())
            {
                mLbLastVisiblePages = vis.values().toVector();
                std::sort(mLbLastVisiblePages.begin(), mLbLastVisiblePages.end());
            }
        }
    }

    QVector<int> candidates;
    candidates.append(1);
    if (mLbUserPage > 1)
    {
        candidates.append(mLbUserPage);
    }
    for (int p : mLbLastVisiblePages)
    {
        if (!candidates.contains(p))
        {
            candidates.append(p);
        }
    }

    for (int page : candidates)
    {
        qint64 elapsed = now - mLbPageFetchTimes.value(page, 0);
        if (elapsed >= 15000)
        {
            LbFetchPage(page);
            FetchNetworkStats();
            return;
        }
    }
}

void MainWindow::AddFinishedRow(int aJobIndex, const JobState &aState)
{
    ui->mFinishedTable->insertRow(0);
    ui->mFinishedTable->setItem(0, 0, new QTableWidgetItem(aState.info.chunkId.left(8)));
    ui->mFinishedTable->setItem(0, 1, new QTableWidgetItem(aState.info.fileId));

    qint64 sizeBytes = aState.total;
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
    cfg.firehoseUrl = ui->mFirehoseUrl->text();
    cfg.serverUrl = ui->mServerUrl->text();
    cfg.token = token;
    cfg.concurrency = ui->mConcurrency->value();
    cfg.subchunkRetries = ui->mSubchunkRetries->value();
    cfg.reconnectDelaySec = ui->mReconnectDelay->value();

    ui->mChunksTable->setRowCount(0);
    mProgressBars.clear();
    mChunkRowIndex.clear();
    mLiveProgress.clear();
    mPendingRemovals.clear();

    if (!resuming)
    {
        mStartTime = QDateTime::currentDateTime();
        mBytesDown = 0;
        mBytesUp = 0;
        mPrevTotal = 0;
        mSmoothSpeed = 0.0;
        ui->mSpeedLabel->setText("--");
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
}

void MainWindow::OnStop()
{
    if (mWorker->IsRunning())
    {
        mWorker->Stop();
    }

    mPaused = false;

    ui->mChunksTable->setRowCount(0);
    mProgressBars.clear();
    mChunkRowIndex.clear();
    ui->mChunksLabel->setText("Active Chunks");
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
    mWorker->CloseConnection();
    QFile::remove(MinervaWorker::TokenPathLocal());
    QFile::remove(avatarCachePath());
    mLoggedInUsername.clear();
    ui->mUserAvatar->hide();
    ui->mRankLabel->setText("--");
    ui->mVerifiedLabel->setText("--");
    RefreshLoginLabel();
    AppendLog("Logged out. Token and avatar cache removed.");
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

    QString apiBase = HyperscrapeProtocol::ApiBaseFromWsUrl(ui->mFirehoseUrl->text().trimmed());
    QNetworkAccessManager *nam = mAvatarNam;

    struct Endpoint { QString url; QLabel *label; };
    QVector<Endpoint> endpoints = {
                                   { apiBase + "/api/leaderboard", ui->mStatusApiLeaderboard },
                                   { apiBase + "/api/stats", ui->mStatusApiStats },
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

        QNetworkReply *reply = nam->get(req);
        QLabel *lbl = ep.label;
        connect(reply, &QNetworkReply::finished, this, [reply, lbl, setStatus, checkDone]()
                {
                    reply->deleteLater();
                    int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    QString err = reply->errorString();
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

    SystemInfo sysInfo = mProfiler->DetectSystemInfo(".");

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
    recForm->addRow("Concurrency:", recConcurrency);
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
            [sysInfo, dlSpeedVal, ulSpeedVal, recConcurrency,
             runSpeedBtn, applyBtn, rec](const SpeedTestResult &result)
            {
                dlSpeedVal->setText(QString("%1 Mbps").arg(result.downloadMbps, 0, 'f', 1));
                ulSpeedVal->setText(QString("%1 Mbps").arg(result.uploadMbps, 0, 'f', 1));
                runSpeedBtn->setEnabled(true);

                *rec = SystemProfiler::Calculate(sysInfo, result);
                recConcurrency->setText(QString::number(rec->concurrency));
                applyBtn->setEnabled(true);
            });

    connect(applyBtn, &QPushButton::clicked, dlg, [this, dlg, rec]()
            {
                ui->mConcurrency->setValue(rec->concurrency);
                AppendLog(QString("Applied recommended settings: concurrency=%1")
                              .arg(rec->concurrency));
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
    FetchNetworkStats();
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
        qint64 liveTotal = mBytesUp;
        for (QMap<int, qint64>::const_iterator it = mLiveProgress.constBegin(); it != mLiveProgress.constEnd(); ++it)
        {
            liveTotal += it.value();
        }

        double inst = std::max(0.0, static_cast<double>(liveTotal - mPrevTotal));
        mPrevTotal = liveTotal;

        constexpr double alpha = 0.3;
        mSmoothSpeed = alpha * inst + (1.0 - alpha) * mSmoothSpeed;

        ui->mSpeedLabel->setText(fmtSpeed(mSmoothSpeed));
        ui->mStreamedLabel->setText(fmtBytes(liveTotal));
    }

    {
        int active = ui->mChunksTable->rowCount();
        ui->mChunksLabel->setText(QString("Active Chunks (%1)").arg(active));
    }
}

void MainWindow::SyncQueueColumns()
{
    int tableW = ui->mChunksTable->viewport()->width();
    if (tableW <= 0)
    {
        return;
    }

    int remaining = tableW - 80 - 100 - 80;
    if (remaining < 300)
    {
        return;
    }

    int statusW = std::max(remaining / 3, 120);
    ui->mChunksTable->setColumnWidth(4, statusW);
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
    if (aToken.isEmpty())
    {
        return;
    }

    QNetworkRequest req(QUrl(QStringLiteral("https://discord.com/api/users/@me")));
    req.setRawHeader("Authorization", ("Bearer " + aToken).toUtf8());
    req.setTransferTimeout(10000);

    QNetworkReply *reply = mAvatarNam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]()
    {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            return;
        }

        QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        QString username = obj.value("global_name").toString();
        if (username.isEmpty())
        {
            username = obj.value("username").toString();
        }
        QString discordId = obj.value("id").toString();
        QString avatarHash = obj.value("avatar").toString();

        if (!username.isEmpty())
        {
            mLoggedInUsername = username;
            ui->mLoginLabel->setText(username);
        }

        QPixmap cached;
        if (cached.load(avatarCachePath()))
        {
            cached = cached.scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            ui->mUserAvatar->setPixmap(cached);
            ui->mUserAvatar->show();
        }

        if (!discordId.isEmpty() && !avatarHash.isEmpty())
        {
            QString ext = avatarHash.startsWith("a_") ? "gif" : "png";
            QString avatarUrl = QString("https://cdn.discordapp.com/avatars/%1/%2.%3")
                                    .arg(discordId, avatarHash, ext);
            QNetworkRequest avatarReq{QUrl(avatarUrl)};
            avatarReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                   QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *avatarReply = mAvatarNam->get(avatarReq);
            connect(avatarReply, &QNetworkReply::finished, this, [this, avatarReply]()
            {
                avatarReply->deleteLater();
                if (avatarReply->error() != QNetworkReply::NoError)
                {
                    return;
                }
                QPixmap pm;
                pm.loadFromData(avatarReply->readAll());
                if (pm.isNull())
                {
                    return;
                }
                pm = pm.scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                pm.save(avatarCachePath(), "PNG");
                ui->mUserAvatar->setPixmap(pm);
                ui->mUserAvatar->show();
            });
        }
    });
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
    ui->mFirehoseUrl->setEnabled(settingsEnabled);
    ui->mServerUrl->setEnabled(settingsEnabled);
    ui->mRecommendBtn->setEnabled(settingsEnabled);
    ui->mConcurrency->setEnabled(settingsEnabled || aRunning);
    ui->mSubchunkRetries->setEnabled(settingsEnabled || aRunning);
    ui->mReconnectDelay->setEnabled(settingsEnabled || aRunning);
    ui->mUiUpdateInterval->setEnabled(settingsEnabled || aRunning);
    if (!aRunning)
    {
        mUptimeTimer->stop();
    }
}

void MainWindow::LoadSettings()
{
    QSettings s(QCoreApplication::applicationDirPath() + "/MinervaStation.ini", QSettings::IniFormat);
    ui->mFirehoseUrl->setText(s.value("firehoseUrl", "wss://firehose.minerva-archive.org/worker").toString());
    ui->mServerUrl->setText(s.value("serverUrl", "https://api.minerva-archive.org").toString());
    ui->mConcurrency->setValue(s.value("concurrency", s.value("dlConcurrency", 5).toInt()).toInt());
    ui->mSubchunkRetries->setValue(s.value("subchunkRetries", 5).toInt());
    ui->mReconnectDelay->setValue(s.value("reconnectDelay", 5).toInt());
    ui->mUiUpdateInterval->setValue(s.value("uiUpdateInterval", 250).toInt());
    mUiUpdateIntervalMs = ui->mUiUpdateInterval->value();
}

void MainWindow::SaveSettings()
{
    QSettings s(QCoreApplication::applicationDirPath() + "/MinervaStation.ini", QSettings::IniFormat);
    s.setValue("firehoseUrl", ui->mFirehoseUrl->text());
    s.setValue("serverUrl", ui->mServerUrl->text());
    s.setValue("concurrency", ui->mConcurrency->value());
    s.setValue("subchunkRetries", ui->mSubchunkRetries->value());
    s.setValue("reconnectDelay", ui->mReconnectDelay->value());
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
    mPendingLogMessages.append(stamped);
    if (mLogFlushTimer && !mLogFlushTimer->isActive())
    {
        mLogFlushTimer->start(100);
    }
}

void MainWindow::FlushLogMessages()
{
    if (mPendingLogMessages.isEmpty())
    {
        return;
    }

    QStringList batch;
    batch.swap(mPendingLogMessages);

    ui->mLogView->appendPlainText(batch.join(QLatin1Char('\n')));
    ui->mLogView->moveCursor(QTextCursor::End);
    ui->mLogView->ensureCursorVisible();

    const QString &last = batch.last();
    int prefixLen = 11;
    statusBar()->showMessage(last.mid(prefixLen), 10000);
}

void MainWindow::FetchNetworkStats()
{
    QString apiBase = HyperscrapeProtocol::ApiBaseFromWsUrl(ui->mFirehoseUrl->text());
    mWorker->FetchStats(apiBase);
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
