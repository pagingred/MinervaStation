#include "splashscreen.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDate>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

SplashScreen::SplashScreen(QWidget *aParent)
    : QSplashScreen()
{
    Q_UNUSED(aParent);

    QPixmap pixmap(WIDTH, HEIGHT);
    pixmap.fill(Qt::transparent);
    setPixmap(pixmap);

    setWindowFlags(Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);

    if (QScreen *screen = QApplication::primaryScreen())
    {
        QRect geo = screen->geometry();
        move((geo.width() - WIDTH) / 2, (geo.height() - HEIGHT) / 2);
    }
}

void SplashScreen::SetStatus(const QString &aMessage)
{
    mStatus = aMessage;
    repaint();
    QApplication::processEvents();
}

void SplashScreen::SetProgress(int aValue, int aMax)
{
    mProgress = aValue;
    mProgressMax = aMax;
    repaint();
    QApplication::processEvents();
}

void SplashScreen::Finish(QWidget *aMainWindow)
{
    if (aMainWindow)
    {
        aMainWindow->show();
    }
    close();
}

void SplashScreen::drawContents(QPainter *aPainter)
{
    aPainter->setRenderHint(QPainter::Antialiasing, true);
    aPainter->setRenderHint(QPainter::TextAntialiasing, true);

    QPainterPath clipPath;
    clipPath.addRoundedRect(0, 0, WIDTH, HEIGHT, 12, 12);
    aPainter->setClipPath(clipPath);

    // Background
    aPainter->setPen(Qt::NoPen);
    aPainter->setBrush(QColor(BG_COLOR));
    aPainter->drawRect(0, 0, WIDTH, HEIGHT);

    // Accent stripe at top
    aPainter->setBrush(QColor(ACCENT_COLOR));
    aPainter->drawRect(0, 0, WIDTH, 6);

    // App icon
    QPixmap icon(":/icon.png");
    if (!icon.isNull())
    {
        QPixmap scaled = icon.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        aPainter->drawPixmap((WIDTH - 48) / 2, 30, scaled);
    }

    // App title — "Minerva" in accent, "Station" in text color
    QFont titleFont("Segoe UI", 28, QFont::Bold);
    aPainter->setFont(titleFont);
    QFontMetrics titleMetrics(titleFont);

    QString nameA = "Minerva";
    QString nameB = "Station";
    int nameAW = titleMetrics.horizontalAdvance(nameA);
    int nameBW = titleMetrics.horizontalAdvance(nameB);
    int totalW = nameAW + nameBW;
    int titleX = (WIDTH - totalW) / 2;
    int titleY = 110;

    aPainter->setPen(QColor(ACCENT_COLOR));
    aPainter->drawText(titleX, titleY, nameA);

    aPainter->setPen(QColor(TEXT_COLOR));
    aPainter->drawText(titleX + nameAW, titleY, nameB);

    // Tagline
    QFont taglineFont("Segoe UI", 10);
    aPainter->setFont(taglineFont);
    aPainter->setPen(QColor(MUTED_COLOR));
    QString tagline = "Distributed Archive Worker";
    QFontMetrics tagMetrics(taglineFont);
    int tagW = tagMetrics.horizontalAdvance(tagline);
    aPainter->drawText((WIDTH - tagW) / 2, titleY + 22, tagline);

    // Version
    QFont verFont("Segoe UI", 9);
    aPainter->setFont(verFont);
    aPainter->setPen(QColor(MUTED_COLOR));
    QString version = QString("Version 1.0.0");
    QFontMetrics verMetrics(verFont);
    int verW = verMetrics.horizontalAdvance(version);
    aPainter->drawText((WIDTH - verW) / 2, titleY + 42, version);

    // Progress bar background
    int progressX = 40;
    int progressY = HEIGHT - 80;
    int progressW = WIDTH - 80;
    int progressH = 6;

    aPainter->setPen(Qt::NoPen);
    aPainter->setBrush(QColor(PANEL_COLOR));
    aPainter->drawRoundedRect(progressX, progressY, progressW, progressH, 3, 3);

    // Progress bar fill
    if (mProgressMax > 0 && mProgress > 0)
    {
        int fillW = (progressW * mProgress) / mProgressMax;
        if (fillW > 0)
        {
            QLinearGradient gradient(progressX, 0, progressX + fillW, 0);
            gradient.setColorAt(0, QColor(ACCENT_COLOR));
            gradient.setColorAt(1, QColor(ACCENT_COLOR).lighter(120));
            aPainter->setBrush(gradient);
            aPainter->drawRoundedRect(progressX, progressY, fillW, progressH, 3, 3);
        }
    }

    // Status text
    if (!mStatus.isEmpty())
    {
        QFont statusFont("Segoe UI", 9);
        aPainter->setFont(statusFont);
        aPainter->setPen(QColor(MUTED_COLOR));
        QFontMetrics statusMetrics(statusFont);
        QString elided = statusMetrics.elidedText(mStatus, Qt::ElideMiddle, progressW);
        aPainter->drawText(progressX, progressY - 8, elided);
    }

    // Organization + year
    QFont orgFont("Segoe UI", 8);
    aPainter->setFont(orgFont);
    aPainter->setPen(QColor(MUTED_COLOR));
    QString org = QCoreApplication::organizationName();
    QFontMetrics orgMetrics(orgFont);
    int orgW = orgMetrics.horizontalAdvance(org);
    aPainter->drawText((WIDTH - orgW) / 2, HEIGHT - 38, org);

    QString year = QString::number(QDate::currentDate().year());
    int yearW = orgMetrics.horizontalAdvance(year);
    aPainter->drawText((WIDTH - yearW) / 2, HEIGHT - 24, year);

    // Border
    aPainter->setClipping(false);
    aPainter->setPen(QPen(QColor(BORDER_COLOR), 1));
    aPainter->setBrush(Qt::NoBrush);
    aPainter->drawRoundedRect(0, 0, WIDTH - 1, HEIGHT - 1, 12, 12);
}
