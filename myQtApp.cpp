/*
 * myQtApp.cpp  –  Android TV Browse UI (first iteration)
 *
 * Design language: Android TV / Material You
 *   - Browse template: horizontal card rows stacked vertically
 *   - Overscan-safe margins: 96 px H / 54 px V  (≈5 % at 1080 p)
 *   - 16:9 cards, 4-wide visible, D-pad navigation
 *   - Focus accent: #A8C7FA (Material You focus blue)
 *
 * Build requirements:
 *   QT += widgets
 *   CONFIG += c++11
 *   (No extra libraries beyond what is already in Qt Widgets.)
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
#include <QDebug>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
//  Design tokens  (tuned for a 1920 × 1080 display)
// ─────────────────────────────────────────────────────────────────────────────
namespace TV {

    // Colours
    const QColor BG         ("#0D0D0D");   // near-black canvas
    const QColor SURFACE    ("#1A1A2E");   // card placeholder base
    const QColor TEXT_PRI   ("#EFEFEF");   // primary text
    const QColor TEXT_SEC   ("#808080");   // secondary / meta text
    const QColor FOCUS_CLR  ("#A8C7FA");   // Material You focus blue

    // Layout  (overscan-safe at 1080 p)
    const int MARGIN_H  =  96;   // left & right safe-zone
    const int MARGIN_V  =  54;   // top & bottom safe-zone

    // Cards
    const int CARD_W    = 380;
    const int CARD_H    = 213;   // 16:9
    const int CARD_R    =   8;   // corner radius
    const int GUTTER    =  20;   // gap between cards
    const int ROW_GAP   =  36;   // gap between rows

} // namespace TV

// ─────────────────────────────────────────────────────────────────────────────
//  Data
// ─────────────────────────────────────────────────────────────────────────────
struct Movie {
    QString imagePath;
    QString title;
    QString meta;       // e.g. "2024  ·  2 h 18 m  ·  Sci-Fi"
    QString filePath;   // passed to mpv; empty = no media yet
};

// ─────────────────────────────────────────────────────────────────────────────
//  MovieCard
// ─────────────────────────────────────────────────────────────────────────────
class MovieCard : public QWidget
{
    Movie        m_data;
    bool         m_focused = false;
    QPixmap      m_thumb;

    // Callback fired when this card gains focus (mouse or keyboard).
    // Lets MainWindow update its info-banner without Q_OBJECT signals.
    std::function<void(MovieCard *)> m_focusCb;

public:
    MovieCard(const Movie &data, QWidget *parent = nullptr)
        : QWidget(parent), m_data(data)
    {
        setFixedSize(TV::CARD_W, TV::CARD_H);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        renderThumb();
    }

    void setFocusCallback(std::function<void(MovieCard *)> cb) { m_focusCb = cb; }
    const Movie &movie() const { return m_data; }

private:
    // ── Render the static thumbnail once into a QPixmap ──────────────────────
    void renderThumb()
    {
        m_thumb = QPixmap(TV::CARD_W, TV::CARD_H);
        m_thumb.fill(Qt::transparent);

        QPainter p(&m_thumb);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

        // Clip everything to a rounded rectangle
        QPainterPath clip;
        clip.addRoundedRect(0, 0, TV::CARD_W, TV::CARD_H, TV::CARD_R, TV::CARD_R);
        p.setClipPath(clip);

        // ── Background: real image or colour-coded placeholder ────────────
        if (!m_data.imagePath.isEmpty() && QFile::exists(m_data.imagePath)) {
            QPixmap src(m_data.imagePath);
            src = src.scaled(TV::CARD_W, TV::CARD_H,
                             Qt::KeepAspectRatioByExpanding,
                             Qt::SmoothTransformation);
            // Centre-crop
            int ox = (src.width()  - TV::CARD_W) / 2;
            int oy = (src.height() - TV::CARD_H) / 2;
            p.drawPixmap(-ox, -oy, src);
        } else {
            // Deterministic hue from the first character of the title
            int hue = m_data.title.isEmpty()
                    ? 210
                    : qAbs(m_data.title[0].unicode() * 53 + 190) % 360;

            QLinearGradient grad(0, 0, TV::CARD_W, TV::CARD_H);
            grad.setColorAt(0.0, QColor::fromHsv(hue,         110, 58));
            grad.setColorAt(1.0, QColor::fromHsv((hue + 45) % 360, 90, 28));
            p.fillRect(0, 0, TV::CARD_W, TV::CARD_H, grad);

            // Large initial letter as a watermark
            p.setFont(QFont("Roboto", 72, QFont::Bold));
            p.setPen(QColor(255, 255, 255, 30));
            p.drawText(QRect(0, -16, TV::CARD_W, TV::CARD_H),
                       Qt::AlignCenter, m_data.title.left(1));
        }

        // ── Bottom scrim so text is readable over any background ──────────
        QLinearGradient scrim(0, TV::CARD_H - 90, 0, TV::CARD_H);
        scrim.setColorAt(0.0, QColor(0, 0, 0,   0));
        scrim.setColorAt(1.0, QColor(0, 0, 0, 230));
        p.fillRect(0, TV::CARD_H - 90, TV::CARD_W, 90, scrim);

        // ── Title ─────────────────────────────────────────────────────────
        QFont titleFont("Roboto", 13, QFont::Bold);
        p.setFont(titleFont);
        p.setPen(TV::TEXT_PRI);
        QString elided = QFontMetrics(titleFont)
                         .elidedText(m_data.title, Qt::ElideRight, TV::CARD_W - 24);
        p.drawText(QRect(12, TV::CARD_H - 60, TV::CARD_W - 24, 26),
                   Qt::AlignLeft | Qt::AlignVCenter, elided);

        // ── Meta line ─────────────────────────────────────────────────────
        p.setFont(QFont("Roboto", 10));
        p.setPen(TV::TEXT_SEC);
        p.drawText(QRect(12, TV::CARD_H - 33, TV::CARD_W - 24, 22),
                   Qt::AlignLeft | Qt::AlignVCenter, m_data.meta);

        p.end();
    }

    // ── Launch the media file with mpv ────────────────────────────────────
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
    // ── Draw glow halo + cached thumbnail + focus border ─────────────────
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (m_focused) {
            // Multi-layer soft glow radiating outward
            for (int i = 7; i >= 1; --i) {
                QPainterPath gp;
                gp.addRoundedRect(QRectF(i, i, width() - 2*i, height() - 2*i),
                                  TV::CARD_R + 2, TV::CARD_R + 2);
                p.setPen(QPen(QColor(168, 199, 250, 10 * i), 2));
                p.drawPath(gp);
            }
        }

        // Thumbnail (pre-rendered, cheap to blit)
        p.drawPixmap(0, 0, m_thumb);

        if (m_focused) {
            // Sharp 3 px focus border on top of the thumbnail
            QPainterPath bp;
            bp.addRoundedRect(QRectF(1.5, 1.5, width() - 3, height() - 3),
                              TV::CARD_R, TV::CARD_R);
            p.setPen(QPen(TV::FOCUS_CLR, 3));
            p.drawPath(bp);
        }
    }

    void focusInEvent(QFocusEvent *e) override
    {
        m_focused = true;
        update();
        if (m_focusCb) m_focusCb(this);
        QWidget::focusInEvent(e);
    }

    void focusOutEvent(QFocusEvent *e) override
    {
        m_focused = false;
        update();
        QWidget::focusOutEvent(e);
    }

    // Enter/Return → play.  Arrow keys → pass up so DpadFilter can catch them.
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
//  Internal row descriptor  (no Q_OBJECT needed)
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
    int               m_r       = 0;
    int               m_c       = 0;
    QLabel           *m_infoTitle = nullptr;
    QLabel           *m_infoMeta  = nullptr;
    QScrollArea      *m_vScroll   = nullptr;

public:
    explicit MainWindow(QWidget *parent = nullptr) : QWidget(parent)
    {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        // Dark canvas; child widgets inherit unless they override.
        setStyleSheet(QString("QWidget { background-color: %1; }")
                      .arg(TV::BG.name()));
        buildUI();
    }

    // Called from main() after show() so focus lands properly.
    void setInitialFocus()
    {
        if (!m_rows.isEmpty() && !m_rows[0].cards.isEmpty())
            m_rows[0].cards[0]->setFocus();
    }

    // ── D-pad navigation (driven by DpadFilter) ───────────────────────────
    void navigate(int dr, int dc)
    {
        // Row clamp
        int nr = qBound(0, m_r + dr, m_rows.size() - 1);

        // When switching rows keep the column position if possible;
        // when moving within a row advance the column.
        int nc = (dr != 0)
               ? qBound(0, m_c,      m_rows[nr].cards.size() - 1)
               : qBound(0, m_c + dc, m_rows[nr].cards.size() - 1);

        m_r = nr;
        m_c = nc;

        MovieCard *card = m_rows[m_r].cards[m_c];
        card->setFocus();

        // Scroll the horizontal strip to reveal the focused card
        if (m_rows[m_r].hScroll)
            m_rows[m_r].hScroll->ensureWidgetVisible(card, TV::GUTTER, 0);

        // Scroll the vertical pane to keep the active row in view
        if (m_vScroll)
            m_vScroll->ensureWidgetVisible(card, 0, TV::ROW_GAP);
    }

    // ── Called by every card when it gains focus ──────────────────────────
    void onCardFocused(MovieCard *card)
    {
        if (m_infoTitle) m_infoTitle->setText(card->movie().title);
        if (m_infoMeta)  m_infoMeta->setText(card->movie().meta);

        // Re-sync cursor in case focus came from a mouse click
        for (int r = 0; r < m_rows.size(); ++r)
            for (int c = 0; c < m_rows[r].cards.size(); ++c)
                if (m_rows[r].cards[c] == card) { m_r = r; m_c = c; return; }
    }

private:
    // ── Build the full widget tree ────────────────────────────────────────
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

        // ── Vertically scrollable region that holds all content rows ──────
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

        // ── Populate your rows here ───────────────────────────────────────
        //    Replace imagePath / filePath with real paths on your target.
        QVector<Movie> library;
        library << Movie{"galactic_quest.png",  "GALACTIC QUEST",
                         "2024  \u00b7  2 h 18 m  \u00b7  Sci-Fi",
                         "/media/big_buck_bunny_1080p_surround.avi"}
                << Movie{"", "JOURNEY TO MARS",
                         "2023  \u00b7  1 h 54 m  \u00b7  Drama",   ""}
                << Movie{"", "THE VOID",
                         "2024  \u00b7  52 m  \u00b7  Documentary", ""}
                << Movie{"", "STELLAR DRIFT",
                         "Series  \u00b7  Season 1  \u00b7  Action", ""}
                << Movie{"", "NOVA RISING",
                         "2022  \u00b7  1 h 42 m  \u00b7  Thriller", ""};
        addRow(vl, "My Library", library);

        QVector<Movie> recent;
        recent << Movie{"", "DEEP HORIZON",
                        "2024  \u00b7  1 h 8 m  \u00b7  Drama",    ""}
               << Movie{"", "ECLIPSE",
                        "2023  \u00b7  24 m  \u00b7  Short Film",  ""}
               << Movie{"", "IRON MERIDIAN",
                        "2024  \u00b7  2 h 2 m  \u00b7  Action",   ""}
               << Movie{"", "BLUE FREQUENCY",
                        "2023  \u00b7  1 h 30 m  \u00b7  Music",   ""};
        addRow(vl, "Recently Added", recent);

        vl->addStretch();
        m_vScroll->setWidget(vc);
        root->addWidget(m_vScroll);
    }

    // ── App header (title + hint text) ────────────────────────────────────
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

        QLabel *hint = new QLabel(
            "\u2190 \u2192  navigate row    "
            "\u2191 \u2193  switch row    "
            "\u23ce  play    "
            "Esc  quit");
        hint->setStyleSheet("color: #444444; font: 13px 'Roboto';"
                            " background: transparent;");
        l->addWidget(hint);
        return w;
    }

    // ── Info banner: updates to reflect whichever card is focused ─────────
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

    // ── Build one horizontal card row ─────────────────────────────────────
    void addRow(QVBoxLayout *parent, const QString &rowTitle,
                const QVector<Movie> &movies)
    {
        CardRow row;

        QWidget *rowW = new QWidget;
        rowW->setStyleSheet("background: transparent;");

        QVBoxLayout *rl = new QVBoxLayout(rowW);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(10);

        // Row heading
        QLabel *lbl = new QLabel(rowTitle);
        lbl->setStyleSheet(QString(
            "color: %1;"
            " font: bold 14px 'Roboto';"
            " letter-spacing: 1px;"
            " background: transparent;")
            .arg(TV::TEXT_PRI.name()));
        rl->addWidget(lbl);

        // Horizontal scroll area  (no visible scrollbar – D-pad drives it)
        QScrollArea *hs = new QScrollArea;
        hs->setFrameShape(QFrame::NoFrame);
        hs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        hs->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        hs->setStyleSheet("background: transparent;");
        // Extra vertical space absorbs the focus glow so it isn't clipped
        hs->setFixedHeight(TV::CARD_H + 18);
        row.hScroll = hs;

        QWidget *strip = new QWidget;
        strip->setStyleSheet("background: transparent;");

        QHBoxLayout *cl = new QHBoxLayout(strip);
        cl->setContentsMargins(4, 9, 4, 9);
        cl->setSpacing(TV::GUTTER);

        for (const Movie &m : movies) {
            MovieCard *card = new MovieCard(m);
            // Bind focus callback without Q_OBJECT / signals
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
    // Esc to quit; arrow keys are handled by DpadFilter before they arrive here.
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape)
            close();
        else
            QWidget::keyPressEvent(e);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  DpadFilter  –  intercepts D-pad / arrow-key events application-wide
//
//  Installing this on QApplication means arrow keys are caught before any
//  QScrollArea or child widget can consume them for its own scrolling.
// ─────────────────────────────────────────────────────────────────────────────
class DpadFilter : public QObject
{
    MainWindow *m_win;
public:
    explicit DpadFilter(MainWindow *win, QObject *parent = nullptr)
        : QObject(parent), m_win(win) {}

    bool eventFilter(QObject * /*obj*/, QEvent *ev) override
    {
        if (ev->type() != QEvent::KeyPress)
            return false;

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
//  Entry point
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // ── Load Roboto from Qt resources (compiled into the binary by qrc) ───
    // This must happen before any QWidget is constructed so that Qt's font
    // resolver can find the family name "Roboto" on the embedded target,
    // regardless of what fonts the rootfs has installed.
    const QStringList fontFiles = {
        ":/fonts/Roboto-Light.ttf",
        ":/fonts/Roboto-Regular.ttf",
        ":/fonts/Roboto-Medium.ttf",
        ":/fonts/Roboto-Bold.ttf"
    };
    for (const QString &path : fontFiles) {
        int id = QFontDatabase::addApplicationFont(path);
        if (id == -1)
            qWarning() << "Failed to load bundled font:" << path;
    }

    // Make Roboto the application-wide default so any widget that does not
    // specify a family explicitly still benefits from the correct typeface.
    QFont appFont("Roboto", 13);
    app.setFont(appFont);

    MainWindow win;

    // Install D-pad filter before the window is visible
    DpadFilter dpad(&win);
    app.installEventFilter(&dpad);

    win.showFullScreen();         // frameless fullscreen – correct for TV / EGLFS
    win.setInitialFocus();        // after show() so focus actually lands

    return app.exec();
}