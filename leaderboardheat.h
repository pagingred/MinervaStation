#ifndef LEADERBOARDHEAT_H
#define LEADERBOARDHEAT_H

#include <QColor>
#include <algorithm>
#include <cmath>
#include <functional>

class LeaderboardHeat
{
public:
    static double BytesToT(qint64 aBytes)
    {
        if (aBytes <= 0)
        {
            return 0.0;
        }
        return std::clamp(std::log1p(static_cast<double>(aBytes)) / std::log1p(HEAT_MAX_BYTES), 0.0, 1.0);
    }

    static QColor ColorForT(double aT)
    {
        aT = std::clamp(aT, 0.0, 1.0);
        int seg = 0;

        for (int s = 0; s < HEAT_STOP_COUNT - 1; ++s)
        {
            if (aT <= HEAT_STOPS[s + 1].pos)
            {
                seg = s;
                break;
            }
            seg = s;
        }

        double frac = (aT - HEAT_STOPS[seg].pos) / (HEAT_STOPS[seg + 1].pos - HEAT_STOPS[seg].pos);
        frac = std::clamp(frac, 0.0, 1.0);

        std::function<int(int, int, double)> lerp = [](int a, int b, double f)
        {
            return static_cast<int>(a + f * (b - a));
        };
        return QColor(lerp(HEAT_STOPS[seg].r, HEAT_STOPS[seg + 1].r, frac),
                      lerp(HEAT_STOPS[seg].g, HEAT_STOPS[seg + 1].g, frac),
                      lerp(HEAT_STOPS[seg].b, HEAT_STOPS[seg + 1].b, frac), 38);
    }

    static QColor FgForT(double aT)
    {
        aT = std::clamp(aT, 0.0, 1.0);
        int seg = 0;

        for (int s = 0; s < HEAT_STOP_COUNT - 1; ++s)
        {
            if (aT <= HEAT_STOPS[s + 1].pos)
            {
                seg = s;
                break;
            }
            seg = s;
        }

        double frac = (aT - HEAT_STOPS[seg].pos) / (HEAT_STOPS[seg + 1].pos - HEAT_STOPS[seg].pos);
        frac = std::clamp(frac, 0.0, 1.0);

        std::function<int(int, int, double)> lerp = [](int a, int b, double f)
        {
            return static_cast<int>(a + f * (b - a));
        };
        std::function<int(int)> brighten = [](int v)
        {
            return std::min(255, v + 90);
        };
        return QColor(brighten(lerp(HEAT_STOPS[seg].r, HEAT_STOPS[seg + 1].r, frac)),
                      brighten(lerp(HEAT_STOPS[seg].g, HEAT_STOPS[seg + 1].g, frac)),
                      brighten(lerp(HEAT_STOPS[seg].b, HEAT_STOPS[seg + 1].b, frac)));
    }

private:
    struct HeatStop
    {
        double pos;
        int r;
        int g;
        int b;
    };

    static constexpr HeatStop HEAT_STOPS[] = {
        {0.00,  58,  60,  82},
        {0.20,  44,  82, 130},
        {0.40,  26, 138, 122},
        {0.60,  56, 168,  72},
        {0.80, 212, 160,  23},
        {1.00, 245, 166,  35},
    };
    static constexpr int HEAT_STOP_COUNT = 6;
    static constexpr double HEAT_MAX_BYTES = 10.0e12;
};

#endif // LEADERBOARDHEAT_H
