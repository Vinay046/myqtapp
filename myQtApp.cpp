/*
 * myQtApp.cpp  –  Android TV Browse UI (second iteration)
 *
 * Changes from first iteration:
 *   1. Hint bar: replaced Unicode arrow glyphs (not in Roboto / no fallback
 *      font on embedded target) with ASCII-safe equivalents drawn via
 *      QPainter so they are 100 % reliable on any rootfs.
 *   2. Focus animation: MovieCard now animates a float m_focusT [0→1] at
 *      ~60 fps using a QTimer, driving both the glow alpha and a subtle
 *      card scale-up (~4 %). No QPropertyAnimation / no extra Qt modules.
 *   3. Xbox controller: GamepadReader opens /dev/input/eventN with
 *      O_NONBLOCK, attaches a QSocketNotifier, and translates evdev
 *      ABS_HAT0X/Y (D-pad) and BTN_SOUTH (A button) into navigate() /
 *      activateFocused() calls on MainWindow.
 *
 * Cross-compilation notes (bitbake / Yocto):
 *   - linux/input.h is part of linux-libc-headers; it is always present in
 *     a Yocto sysroot.  No extra DEPENDS entry is needed in the recipe.
 *   - Because GamepadReader and MovieCard both carry Q_OBJECT, qmake will
 *     invoke moc for myQtApp.cpp automatically.  The generated file is
 *     included at the bottom of this translation unit with
 *       #include "myQtApp.moc"
 *     This is the standard single-file moc pattern; qmake supports it.
 *   - /dev/input/eventN must be readable by the user running the app.
 *     Add the user to the "input" group in your image, or set
 *       SUBSYSTEM=="input", GROUP="input", MODE="0660"
 *     in a udev rule deployed by your layer.
 *
 * Build:
 *   QT += widgets
 *   CONFIG += c++11
 *   SOURCES += myQtApp.cpp
 *   RESOURCES += fonts.qrc
 */

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QLinearGradient>
#include <QFontMetrics>
#include <QFile>
#include <QProcess>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QFocusEvent>
#include <QPaintEvent>
#include <QFontDatabase>
#include <QTimer>
#include <QSocketNotifier>
#include <QDebug>
#include <functional>

// evdev – always available in a Yocto sysroot via linux-libc-headers
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  Design tokens  (tuned for 1920 × 1080)
// ─────────────────────────────────────────────────────────────────────────────
namespace TV {

    const QColor BG        ("#0D0D0D");
    const QColor SURFACE   ("#1A1A2E");
    const QColor TEXT_PRI  ("#EFEFEF");
    const QColor TEXT_SEC  ("#808080");
    const QColor FOCUS_CLR ("#A8C7FA");   // Material You focus blue

    const int MARGIN_H = 96;
    const int MARGIN_V = 54;

    const int CARD_W   = 380;
    const int CARD_H   = 213;   // 16:9
    const int CARD_R   =   8;
    const int GUTTER   =  20;
    const int ROW_GAP  =  36;

    // Focus animation
    const float ANIM_STEP_IN  = 0.14f;   // speed coming in  (0→1)
    const float ANIM_STEP_OUT = 0.10f;   // speed going out  (1→0)
    const int   ANIM_MS       =  16;     // ~60 fps

    // Scale applied to the card when fully focused
    const float FOCUS_SCALE   = 1.04f;

} // namespace TV

// ─────────────────────────────────────────────────────────────────────────────
//  Data
// ─────────────────────────────────────────────────────────────────────────────
struct Movie {
    QString imagePath;
    QString title;
    QString meta;
    QString filePath;
};

// Forward declaration so GamepadReader can call MainWindow methods.
class MainWindow;

// ─────────────────────────────────────────────────────────────────────────────
//  MovieCard
// ─────────────────────────────────────────────────────────────────────────────
class MovieCard : public QWidget
{
    Q_OBJECT   // required for QTimer::timeout lambda connections

    Movie       m_data;
    bool        m_focused = false;
    float       m_focusT  = 0.0f;   // animation progress [0,1]
    QPixmap     m_thumb;
    QTimer     *m_animTimer = nullptr;

    std::function<void(MovieCard *)> m_focusCb;

public:
    MovieCard(const Movie &data, QWidget *parent = nullptr)
        : QWidget(parent), m_data(data)
    {
        // Extra margin absorbs the scale-up so the glow is never clipped.
        const int PAD = 12;
        setFixedSize(TV::CARD_W + PAD * 2, TV::CARD_H + PAD * 2);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);

        // Pre-render the static thumbnail once.
        renderThumb();

        // Animation timer (stopped by default).
        m_animTimer = new QTimer(this);
        m_animTimer->setInterval(TV::ANIM_MS);
        connect(m_animTimer, &QTimer::timeout, this, [this]() {
            float target = m_focused ? 1.0f : 0.0f;
            float step   = m_focused ? TV::ANIM_STEP_IN : TV::ANIM_STEP_OUT;
            if (qAbs(m_focusT - target) <= step) {
                m_focusT = target;
                m_animTimer->stop();
            } else {
                m_focusT += (m_focused ? step : -step);
            }
            update();
        });
    }

    void setFocusCallback(std::function<void(MovieCard *)> cb) { m_focusCb = cb; }
    const Movie &movie() const { return m_data; }

    void triggerLaunch() { launch(); }

private:
    // ── Pre-render thumbnail ──────────────────────────────────────────────
    void renderThumb()
    {
        m_thumb = QPixmap(TV::CARD_W, TV::CARD_H);
        m_thumb.fill(Qt::transparent);

        QPainter p(&m_thumb);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

        QPainterPath clip;
        clip.addRoundedRect(0, 0, TV::CARD_W, TV::CARD_H, TV::CARD_R, TV::CARD_R);
        p.setClipPath(clip);

        if (!m_data.imagePath.isEmpty() && QFile::exists(m_data.imagePath)) {
            QPixmap src(m_data.imagePath);
            src = src.scaled(TV::CARD_W, TV::CARD_H,
                             Qt::KeepAspectRatioByExpanding,
                             Qt::SmoothTransformation);
            int ox = (src.width()  - TV::CARD_W) / 2;
            int oy = (src.height() - TV::CARD_H) / 2;
            p.drawPixmap(-ox, -oy, src);
        } else {
            int hue = m_data.title.isEmpty()
                    ? 210
                    : qAbs(m_data.title[0].unicode() * 53 + 190) % 360;
            QLinearGradient grad(0, 0, TV::CARD_W, TV::CARD_H);
            grad.setColorAt(0.0, QColor::fromHsv(hue,           110, 58));
            grad.setColorAt(1.0, QColor::fromHsv((hue + 45) % 360, 90, 28));
            p.fillRect(0, 0, TV::CARD_W, TV::CARD_H, grad);

            p.setFont(QFont("Roboto", 72, QFont::Bold));
            p.setPen(QColor(255, 255, 255, 30));
            p.drawText(QRect(0, -16, TV::CARD_W, TV::CARD_H),
                       Qt::AlignCenter, m_data.title.left(1));
        }

        QLinearGradient scrim(0, TV::CARD_H - 90, 0, TV::CARD_H);
        scrim.setColorAt(0.0, QColor(0, 0, 0,   0));
        scrim.setColorAt(1.0, QColor(0, 0, 0, 230));
        p.fillRect(0, TV::CARD_H - 90, TV::CARD_W, 90, scrim);

        QFont titleFont("Roboto", 13, QFont::Bold);
        p.setFont(titleFont);
        p.setPen(TV::TEXT_PRI);
        QString elided = QFontMetrics(titleFont)
                         .elidedText(m_data.title, Qt::ElideRight, TV::CARD_W - 24);
        p.drawText(QRect(12, TV::CARD_H - 60, TV::CARD_W - 24, 26),
                   Qt::AlignLeft | Qt::AlignVCenter, elided);

        p.setFont(QFont("Roboto", 10));
        p.setPen(TV::TEXT_SEC);
        p.drawText(QRect(12, TV::CARD_H - 33, TV::CARD_W - 24, 22),
                   Qt::AlignLeft | Qt::AlignVCenter, m_data.meta);

        p.end();
    }

    void launch()
    {
        if (!m_data.filePath.isEmpty() && QFile::exists(m_data.filePath)) {
            qDebug() << "Launching:" << m_data.title;
            QProcess::startDetached("mpv", QStringList()
                << "-v"
                << "--vo=gpu"
                << "--audio-device=alsa/sysdefault:CARD=hdmi0"
                << m_data.filePath);
        } else {
            qDebug() << "No media file for:" << m_data.title;
        }
    }

protected:
    // ── Animated paint: glow + scaled thumbnail + focus border ───────────
    void paintEvent(QPaintEvent *) override
    {
        const int PAD = (width()  - TV::CARD_W) / 2;   // == 12
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (m_focusT > 0.001f) {
            // ── Soft multi-layer glow ─────────────────────────────────────
            for (int i = 8; i >= 1; --i) {
                QPainterPath gp;
                QRectF gr(PAD - i, PAD - i,
                          TV::CARD_W + 2*i, TV::CARD_H + 2*i);
                gp.addRoundedRect(gr, TV::CARD_R + 3, TV::CARD_R + 3);
                int alpha = static_cast<int>(m_focusT * 11 * i);
                p.setPen(QPen(QColor(168, 199, 250, alpha), 2));
                p.drawPath(gp);
            }
        }

        // ── Scale the card toward the viewer when focused ─────────────────
        float scale = 1.0f + (TV::FOCUS_SCALE - 1.0f) * m_focusT;
        p.save();
        p.translate(PAD + TV::CARD_W / 2.0, PAD + TV::CARD_H / 2.0);
        p.scale(scale, scale);
        p.translate(-(TV::CARD_W / 2.0), -(TV::CARD_H / 2.0));
        p.drawPixmap(0, 0, m_thumb);
        p.restore();

        if (m_focusT > 0.001f) {
            // ── Sharp focus border ────────────────────────────────────────
            p.save();
            p.translate(PAD + TV::CARD_W / 2.0, PAD + TV::CARD_H / 2.0);
            p.scale(scale, scale);
            p.translate(-(TV::CARD_W / 2.0), -(TV::CARD_H / 2.0));

            QPainterPath bp;
            bp.addRoundedRect(QRectF(1.5, 1.5, TV::CARD_W - 3, TV::CARD_H - 3),
                              TV::CARD_R, TV::CARD_R);
            QColor borderClr = TV::FOCUS_CLR;
            borderClr.setAlphaF(m_focusT);
            p.setPen(QPen(borderClr, 3));
            p.drawPath(bp);
            p.restore();
        }
    }

    void focusInEvent(QFocusEvent *e) override
    {
        m_focused = true;
        m_animTimer->start();
        if (m_focusCb) m_focusCb(this);
        QWidget::focusInEvent(e);
    }

    void focusOutEvent(QFocusEvent *e) override
    {
        m_focused = false;
        m_animTimer->start();   // animate back to 0
        QWidget::focusOutEvent(e);
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
            launch();
        else
            e->ignore();
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        setFocus();
        launch();
        QWidget::mousePressEvent(e);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  HintBar  –  draws the navigation hint entirely with QPainter.
//  Avoids relying on any Unicode codepoint outside the Basic Latin block,
//  so it is immune to missing-glyph boxes on embedded targets.
// ─────────────────────────────────────────────────────────────────────────────
class HintBar : public QWidget
{
public:
    explicit HintBar(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(28);
        setAttribute(Qt::WA_TranslucentBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Dim colour for the whole bar
        const QColor dim(0x44, 0x44, 0x44);
        p.setFont(QFont("Roboto", 12));
        p.setPen(dim);

        // Helper: draw a small triangle arrow
        auto arrow = [&](int cx, int cy, int dir) {
            // dir: 0=left, 1=right, 2=up, 3=down
            const int S = 6;   // half-size
            QPolygon tri;
            switch (dir) {
            case 0: tri << QPoint(cx + S, cy - S)
                        << QPoint(cx - S, cy)
                        << QPoint(cx + S, cy + S); break;
            case 1: tri << QPoint(cx - S, cy - S)
                        << QPoint(cx + S, cy)
                        << QPoint(cx - S, cy + S); break;
            case 2: tri << QPoint(cx - S, cy + S)
                        << QPoint(cx,     cy - S)
                        << QPoint(cx + S, cy + S); break;
            case 3: tri << QPoint(cx - S, cy - S)
                        << QPoint(cx,     cy + S)
                        << QPoint(cx + S, cy - S); break;
            }
            p.setBrush(dim);
            p.setPen(Qt::NoPen);
            p.drawPolygon(tri);
            p.setPen(dim);
        };

        const int cy = height() / 2;
        int x = 0;

        // ← → navigate row
        arrow(x + 8,  cy, 0); x += 20;
        arrow(x,      cy, 1); x += 16;
        p.drawText(x, cy + 5, "navigate row"); x += 115;

        // ↑ ↓ switch row
        arrow(x + 8, cy - 5, 2); x += 20;
        arrow(x,     cy + 4, 3); x += 18;
        p.drawText(x, cy + 5, "switch row"); x += 95;

        // [A] play  (Xbox A button icon)
        p.setBrush(Qt::NoBrush);
        p.setPen(dim);
        p.drawEllipse(QPoint(x + 9, cy), 8, 8);
        p.setFont(QFont("Roboto", 10, QFont::Bold));
        p.drawText(QRect(x + 1, cy - 8, 17, 17), Qt::AlignCenter, "A");
        x += 28;
        p.setFont(QFont("Roboto", 12));
        p.drawText(x, cy + 5, "play"); x += 52;

        // Esc quit
        p.drawText(x, cy + 5, "Esc  quit");
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Internal row descriptor
// ─────────────────────────────────────────────────────────────────────────────
struct CardRow {
    QVector<MovieCard *> cards;
    QScrollArea         *hScroll = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
//  MainWindow
// ─────────────────────────────────────────────────────────────────────────────
class MainWindow : public QWidget
{
    QVector<CardRow>  m_rows;
    int               m_r        = 0;
    int               m_c        = 0;
    QLabel           *m_infoTitle = nullptr;
    QLabel           *m_infoMeta  = nullptr;
    QScrollArea      *m_vScroll   = nullptr;

public:
    explicit MainWindow(QWidget *parent = nullptr) : QWidget(parent)
    {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setStyleSheet(QString("QWidget { background-color: %1; }")
                      .arg(TV::BG.name()));
        buildUI();
    }

    void setInitialFocus()
    {
        if (!m_rows.isEmpty() && !m_rows[0].cards.isEmpty())
            m_rows[0].cards[0]->setFocus();
    }

    // D-pad / joystick navigation
    void navigate(int dr, int dc)
    {
        int nr = qBound(0, m_r + dr, m_rows.size() - 1);
        int nc = (dr != 0)
               ? qBound(0, m_c,      m_rows[nr].cards.size() - 1)
               : qBound(0, m_c + dc, m_rows[nr].cards.size() - 1);
        m_r = nr;
        m_c = nc;

        MovieCard *card = m_rows[m_r].cards[m_c];
        card->setFocus();

        if (m_rows[m_r].hScroll)
            m_rows[m_r].hScroll->ensureWidgetVisible(card, TV::GUTTER, 0);
        if (m_vScroll)
            m_vScroll->ensureWidgetVisible(card, 0, TV::ROW_GAP);
    }

    // Called by GamepadReader when the A button is pressed
    void activateFocused()
    {
        if (m_r < m_rows.size() && m_c < m_rows[m_r].cards.size())
            m_rows[m_r].cards[m_c]->triggerLaunch();
    }

    void onCardFocused(MovieCard *card)
    {
        if (m_infoTitle) m_infoTitle->setText(card->movie().title);
        if (m_infoMeta)  m_infoMeta->setText(card->movie().meta);

        for (int r = 0; r < m_rows.size(); ++r)
            for (int c = 0; c < m_rows[r].cards.size(); ++c)
                if (m_rows[r].cards[c] == card) { m_r = r; m_c = c; return; }
    }

private:
    void buildUI()
    {
        QVBoxLayout *root = new QVBoxLayout(this);
        root->setContentsMargins(TV::MARGIN_H, TV::MARGIN_V,
                                 TV::MARGIN_H, TV::MARGIN_V);
        root->setSpacing(0);

        root->addWidget(makeHeader());
        root->addSpacing(20);
        root->addWidget(makeInfoBanner());
        root->addSpacing(24);

        m_vScroll = new QScrollArea;
        m_vScroll->setFrameShape(QFrame::NoFrame);
        m_vScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_vScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_vScroll->setStyleSheet("background: transparent;");
        m_vScroll->setWidgetResizable(true);

        QWidget *vc = new QWidget;
        vc->setStyleSheet("background: transparent;");
        QVBoxLayout *vl = new QVBoxLayout(vc);
        vl->setContentsMargins(0, 0, 0, 0);
        vl->setSpacing(TV::ROW_GAP);

        QVector<Movie> library;
        library << Movie{"galactic_quest.png", "GALACTIC QUEST",
                         "2024  \u00b7  2 h 18 m  \u00b7  Sci-Fi",
                         "/media/big_buck_bunny_1080p_surround.avi"}
                << Movie{"", "JOURNEY TO MARS",
                         "2023  \u00b7  1 h 54 m  \u00b7  Drama",    ""}
                << Movie{"", "THE VOID",
                         "2024  \u00b7  52 m  \u00b7  Documentary",  ""}
                << Movie{"", "STELLAR DRIFT",
                         "Series  \u00b7  Season 1  \u00b7  Action", ""}
                << Movie{"", "NOVA RISING",
                         "2022  \u00b7  1 h 42 m  \u00b7  Thriller", ""};
        addRow(vl, "My Library", library);

        QVector<Movie> recent;
        recent << Movie{"", "DEEP HORIZON",
                        "2024  \u00b7  1 h 8 m  \u00b7  Drama",   ""}
               << Movie{"", "ECLIPSE",
                        "2023  \u00b7  24 m  \u00b7  Short Film", ""}
               << Movie{"", "IRON MERIDIAN",
                        "2024  \u00b7  2 h 2 m  \u00b7  Action",  ""}
               << Movie{"", "BLUE FREQUENCY",
                        "2023  \u00b7  1 h 30 m  \u00b7  Music",  ""};
        addRow(vl, "Recently Added", recent);

        vl->addStretch();
        m_vScroll->setWidget(vc);
        root->addWidget(m_vScroll);
    }

    QWidget *makeHeader()
    {
        QWidget *w = new QWidget;
        w->setStyleSheet("background: transparent;");
        w->setFixedHeight(52);

        QHBoxLayout *l = new QHBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);

        QLabel *appName = new QLabel("MY FILE LIBRARY");
        appName->setStyleSheet(QString(
            "color: %1;"
            " font: bold 26px 'Roboto';"
            " letter-spacing: 4px;"
            " background: transparent;")
            .arg(TV::TEXT_PRI.name()));
        l->addWidget(appName);
        l->addStretch();

        // Custom-drawn hint bar; never shows missing-glyph boxes.
        l->addWidget(new HintBar);
        return w;
    }

    QWidget *makeInfoBanner()
    {
        QWidget *w = new QWidget;
        w->setStyleSheet("background: transparent;");
        w->setFixedHeight(40);

        QHBoxLayout *l = new QHBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(18);

        m_infoTitle = new QLabel("\u2014");
        m_infoTitle->setStyleSheet(QString(
            "color: %1; font: bold 18px 'Roboto'; background: transparent;")
            .arg(TV::TEXT_PRI.name()));

        m_infoMeta = new QLabel("");
        m_infoMeta->setStyleSheet(QString(
            "color: %1; font: 13px 'Roboto'; background: transparent;")
            .arg(TV::TEXT_SEC.name()));

        l->addWidget(m_infoTitle);
        l->addWidget(m_infoMeta);
        l->addStretch();
        return w;
    }

    void addRow(QVBoxLayout *parent, const QString &rowTitle,
                const QVector<Movie> &movies)
    {
        CardRow row;

        QWidget *rowW = new QWidget;
        rowW->setStyleSheet("background: transparent;");

        QVBoxLayout *rl = new QVBoxLayout(rowW);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(10);

        QLabel *lbl = new QLabel(rowTitle);
        lbl->setStyleSheet(QString(
            "color: %1;"
            " font: bold 14px 'Roboto';"
            " letter-spacing: 1px;"
            " background: transparent;")
            .arg(TV::TEXT_PRI.name()));
        rl->addWidget(lbl);

        QScrollArea *hs = new QScrollArea;
        hs->setFrameShape(QFrame::NoFrame);
        hs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        hs->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        hs->setStyleSheet("background: transparent;");
        // Taller to accommodate scale-up + glow bleed
        hs->setFixedHeight(TV::CARD_H + 40);
        row.hScroll = hs;

        QWidget *strip = new QWidget;
        strip->setStyleSheet("background: transparent;");

        QHBoxLayout *cl = new QHBoxLayout(strip);
        cl->setContentsMargins(4, 14, 4, 14);
        cl->setSpacing(TV::GUTTER - 4);   // slightly tighter; cards are wider with PAD

        for (const Movie &m : movies) {
            MovieCard *card = new MovieCard(m);
            card->setFocusCallback([this](MovieCard *c){ onCardFocused(c); });
            cl->addWidget(card);
            row.cards.append(card);
        }
        cl->addStretch();

        hs->setWidget(strip);
        rl->addWidget(hs);
        parent->addWidget(rowW);
        m_rows.append(row);
    }

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape)
            close();
        else
            QWidget::keyPressEvent(e);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  DpadFilter  –  keyboard arrow-key intercept (unchanged from v1)
// ─────────────────────────────────────────────────────────────────────────────
class DpadFilter : public QObject
{
    MainWindow *m_win;
public:
    explicit DpadFilter(MainWindow *win, QObject *parent = nullptr)
        : QObject(parent), m_win(win) {}

    bool eventFilter(QObject * /*obj*/, QEvent *ev) override
    {
        if (ev->type() != QEvent::KeyPress) return false;
        QKeyEvent *ke = static_cast<QKeyEvent *>(ev);
        switch (ke->key()) {
        case Qt::Key_Left:  m_win->navigate( 0, -1); return true;
        case Qt::Key_Right: m_win->navigate( 0,  1); return true;
        case Qt::Key_Up:    m_win->navigate(-1,  0); return true;
        case Qt::Key_Down:  m_win->navigate( 1,  0); return true;
        default:            return false;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  GamepadReader  –  evdev Xbox controller input
//
//  Reads raw struct input_event from /dev/input/eventN with O_NONBLOCK.
//  QSocketNotifier wakes the event loop the moment data is available.
//
//  Xbox One / 360 wired / wireless (xpad kernel module) mapping:
//    EV_ABS  ABS_HAT0X   -1 = left,  +1 = right
//    EV_ABS  ABS_HAT0Y   -1 = up,    +1 = down
//    EV_KEY  BTN_SOUTH    1 = A pressed  (select / play)
//    EV_KEY  BTN_EAST     1 = B pressed  (could be used for back/quit)
//
//  The Xbox controller must be bound by the xpad module (included in most
//  Yocto kernels via linux-yocto's xbox configuration fragment).
//  Verify with:  cat /proc/bus/input/devices | grep -A6 Xbox
// ─────────────────────────────────────────────────────────────────────────────
class GamepadReader : public QObject
{
    Q_OBJECT
    MainWindow      *m_win;
    int              m_fd       = -1;
    QSocketNotifier *m_notifier = nullptr;

public:
    GamepadReader(const QString &devPath, MainWindow *win,
                  QObject *parent = nullptr)
        : QObject(parent), m_win(win)
    {
        m_fd = ::open(devPath.toLocal8Bit().constData(),
                      O_RDONLY | O_NONBLOCK);
        if (m_fd < 0) {
            qWarning() << "GamepadReader: cannot open" << devPath
                       << "-" << ::strerror(errno);
            qWarning() << "  Make sure /dev/input/event* is readable "
                          "(add user to 'input' group or adjust udev rules).";
            return;
        }
        qDebug() << "GamepadReader: opened" << devPath;

        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated,
                this, &GamepadReader::onData);
    }

    ~GamepadReader() override
    {
        if (m_fd >= 0) ::close(m_fd);
    }

private slots:
    void onData()
    {
        struct input_event ev;
        // Drain all pending events in one call so we never fall behind.
        while (::read(m_fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev))) {

            if (ev.type == EV_ABS) {
                // D-pad hat switch
                if (ev.code == ABS_HAT0X) {
                    if      (ev.value == -1) m_win->navigate(0, -1);   // left
                    else if (ev.value ==  1) m_win->navigate(0,  1);   // right
                } else if (ev.code == ABS_HAT0Y) {
                    if      (ev.value == -1) m_win->navigate(-1, 0);   // up
                    else if (ev.value ==  1) m_win->navigate( 1, 0);   // down
                }
            } else if (ev.type == EV_KEY && ev.value == 1 /*key-down*/) {
                switch (ev.code) {
                case BTN_SOUTH:                        // A button → play
                    m_win->activateFocused();
                    break;
                case BTN_EAST:                         // B button → quit
                    qDebug() << "GamepadReader: B pressed – closing";
                    // Post a close event; safe to call from the event loop.
                    QMetaObject::invokeMethod(m_win, "close",
                                             Qt::QueuedConnection);
                    break;
                default:
                    break;
                }
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // Load Roboto from Qt resources before any QWidget is constructed.
    const QStringList fontFiles = {
        ":/fonts/Roboto-Light.ttf",
        ":/fonts/Roboto-Regular.ttf",
        ":/fonts/Roboto-Medium.ttf",
        ":/fonts/Roboto-Bold.ttf"
    };
    for (const QString &path : fontFiles) {
        if (QFontDatabase::addApplicationFont(path) == -1)
            qWarning() << "Failed to load bundled font:" << path;
    }
    app.setFont(QFont("Roboto", 13));

    MainWindow win;

    // Keyboard D-pad filter (arrow keys)
    DpadFilter kbDpad(&win);
    app.installEventFilter(&kbDpad);

    // Xbox controller via evdev.
    // Pass a different path on the command line if the device index differs:
    //   ./myqtapp /dev/input/event3
    QString gamepadPath = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                     : QStringLiteral("/dev/input/event2");
    GamepadReader gamepad(gamepadPath, &win);

    win.showFullScreen();
    win.setInitialFocus();

    return app.exec();
}

// ── Required for Q_OBJECT in a .cpp translation unit ─────────────────────────
// qmake generates myQtApp.moc when it sees Q_OBJECT inside myQtApp.cpp.
#include "myQtApp.moc"