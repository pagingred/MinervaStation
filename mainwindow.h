#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "minervaworker.h"
#include "lbentry.h"

#include <QMainWindow>
#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <QPixmap>

class QTimer;
class QNetworkAccessManager;
class QJsonArray;
class QProgressBar;
class QTableWidgetItem;
class SystemProfiler;

namespace Ui
{
    class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *aParent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *aEvent) override;
    void resizeEvent(QResizeEvent *aEvent) override;
    bool eventFilter(QObject *aObj, QEvent *aEvent) override;

private slots:
    void OnStart();
    void OnPause();
    void OnStop();
    void OnLogin();
    void OnLogout();
    void OnRefreshLeaderboard();
    void OnCheckServers();
    void OnRecommendedSettings();
    void UpdateUptime();

private:
    void SetupTables();
    void SetupConnections();
    void LoadSettings();
    void SaveSettings();
    void RefreshLoginLabel();
    void FetchUserProfile(const QString &aToken);
    void StartRefreshCooldown();
    void SetRunningUi(bool aRunning);
    void AddFinishedRow(int aJobIndex, const JobState &aState);
    void SyncQueueColumns();
    void AppendLog(const QString &aMsg);
    void FlushLogMessages();

    void LbAppendPageData(const QJsonArray &aRows, int aPage);
    void LbRefreshDisplay();
    void LbRefreshVisible();
    void LbFetchNextPageIfNeeded();
    void LbFetchPage(int aPage);
    void FetchNetworkStats();

    Ui::MainWindow *ui;

    MinervaWorker *mWorker;
    QTimer *mUptimeTimer;
    QTimer *mLbTimer;
    QTimer *mLbCooldown;
    QDateTime mStartTime;
    bool mPaused = false;

    QMap<int, QProgressBar*> mProgressBars;
    QMap<int, QPair<qint64, JobState>> mPendingRemovals;

    QHash<int, QTableWidgetItem*> mChunkRowIndex;

    int mFinishedDoneCount = 0;
    int mFinishedFailCount = 0;

    int mLbCooldownSecs = 0;
    QVector<LbEntry> mLbAllRows;
    int mLbHighestPage = 0;
    bool mLbFetchingPage = false;
    bool mLbAllPagesLoaded = false;
    bool mLbDisplaying = false;
    int mLbRealRowCount = 0;
    QMap<int, qint64> mLbPageFetchTimes;

    int mLbSortCol = 0;
    bool mLbSortAsc = true;
    int mLbUserPage = 1;
    QVector<int> mLbLastVisiblePages;

    QNetworkAccessManager *mAvatarNam;
    QMap<QString, QPixmap> mAvatarCache;
    QString mLoggedInUsername;

    SystemProfiler *mProfiler = nullptr;

    qint64 mBytesDown = 0;
    qint64 mBytesUp = 0;
    QMap<int, qint64> mLiveProgress;

    qint64 mPrevTotal = 0;
    double mSmoothSpeed = 0.0;

    QElapsedTimer mUiClock;
    QMap<int, qint64> mLastUiUpdate;
    qint64 mUiUpdateIntervalMs = 250;

    QHash<int, JobState> mPendingJobStates;
    QTimer *mFlushTimer = nullptr;
    void FlushJobUpdates();

    QStringList mPendingLogMessages;
    QTimer *mLogFlushTimer = nullptr;
};

#endif // MAINWINDOW_H
