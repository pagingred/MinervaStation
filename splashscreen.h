#ifndef SPLASHSCREEN_H
#define SPLASHSCREEN_H

#include <QSplashScreen>
#include <QColor>

class SplashScreen : public QSplashScreen
{
    Q_OBJECT

public:
    explicit SplashScreen(QWidget *aParent = nullptr);

    void SetStatus(const QString &aMessage);
    void SetProgress(int aValue, int aMax = 100);
    void Finish(QWidget *aMainWindow);

protected:
    void drawContents(QPainter *aPainter) override;

private:
    QString mStatus;
    int mProgress = 0;
    int mProgressMax = 100;

    static constexpr int WIDTH = 480;
    static constexpr int HEIGHT = 300;

    static constexpr char BG_COLOR[] = "#1e1e1e";
    static constexpr char PANEL_COLOR[] = "#2d2d2d";
    static constexpr char ACCENT_COLOR[] = "#2980b9";
    static constexpr char TEXT_COLOR[] = "#d4d4d4";
    static constexpr char MUTED_COLOR[] = "#888888";
    static constexpr char BORDER_COLOR[] = "#444444";
};

#endif // SPLASHSCREEN_H
