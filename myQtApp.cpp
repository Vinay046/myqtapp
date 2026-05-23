/*
 * myQtApp.cpp  –  Android TV Browse UI  v2
 *
 * What's new in v2:
 *  1. Left sidebar overlay (BTN_START / Qt::Key_Home)
 *       - Categories: Search, Movies, Series, Music
 *       - D-pad navigates items; Enter selects; Left/Esc dismisses
 *       - User avatar, "Settings" gutter entry, active-item pill highlight
 *  2. Detail overlay (Movies & Series)
 *       - Full-screen dim + centred rounded card (image, title, meta, desc)
 *       - Two D-pad-navigable buttons: Play and Add to Favorites
 *       - Click outside card to dismiss
 *  3. Music cards
 *       - Square (220×220) album-art tiles
 *       - Focused state shows a white-circle play-button overlay + glow border
 *       - Enter / A-button plays the file directly (no detail overlay)
 *  4. Unified AppState machine routes every key press from DpadFilter
 *       Browse → Sidebar → Detail transitions are clean and reversible
 *
 * Controller mapping (evdev / xpad kernel module):
 *  BTN_START (code 314)  → open / close sidebar  (also Qt::Key_Home)
 *  ABS_HAT0X -1/+1       → left / right
 *  ABS_HAT0Y -1/+1       → up / down
 *  BTN_SOUTH (A button)  → confirm / play
 *  BTN_EAST  (B button)  → back / dismiss
 *
 * Cross-compile / Bitbake notes:
 *  - QT += widgets          (no extra modules)
 *  - CONFIG += c++11
 *  - RESOURCES += fonts.qrc
 *  - linux/input.h          from linux-libc-headers in any Yocto sysroot
 *  - <cmath>                standard C++11 (used for gear icon only)
 *  - No Q_OBJECT macro on custom classes → no per-class moc step
 *    (std::function<> callbacks replace signals/slots throughout)
 *  - GamepadReader uses QSocketNotifier (unchanged from v1)
 *  - /dev/input/eventN must be readable; see udev notes in v1
 */

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QLineEdit>
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
#include <QResizeEvent>
#include <QFontDatabase>
#include <QSocketNotifier>
#include <QDebug>
#include <QPropertyAnimation>
#include <QParallelAnimationGroup>
#include <QGraphicsOpacityEffect>
#include <QEasingCurve>
#include <functional>
#include <cmath>

#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  Design tokens  (1920×1080)
// ─────────────────────────────────────────────────────────────────────────────
namespace TV {
    const QColor BG        ("#0D0D0D");
    const QColor SURFACE   ("#1C1C1E");
    const QColor TEXT_PRI  ("#EFEFEF");
    const QColor TEXT_SEC  ("#888888");
    const QColor FOCUS_CLR ("#A8C7FA");

    const int MARGIN_H  =  96;
    const int MARGIN_V  =  54;

    const int CARD_W    = 380;
    const int CARD_H    = 213;  // 16:9
    const int CARD_R    =  10;
    const int GUTTER    =  20;
    const int ROW_GAP   =  40;

    const int MUSIC_SZ  = 220;  // square music tile
    const int SIDEBAR_W = 380;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Enums & data structs
// ─────────────────────────────────────────────────────────────────────────────
enum class AppState { Browse, Sidebar, Detail };
enum class Category { Search, Movies, Series, Music };
enum class CardKind  { Movie, Series, Music };

struct ContentItem {
    CardKind kind;
    QString  imagePath;
    QString  title;
    QString  meta;
    QString  description;
    QString  filePath;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Icon painters  (each saves/restores painter state)
// ─────────────────────────────────────────────────────────────────────────────
static void drawSearchIcon(QPainter &p, QRectF r, QColor c)
{
    p.save();
    p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    qreal cx = r.left() + r.width()*.40,  cy = r.top() + r.height()*.40;
    qreal cr = r.width()*.30;
    p.drawEllipse(QPointF(cx,cy), cr, cr);
    qreal s = cr*.707;
    p.drawLine(QPointF(cx+s,cy+s), QPointF(r.right()-1,r.bottom()-1));
    p.restore();
}

static void drawMoviesIcon(QPainter &p, QRectF r, QColor c)
{
    p.save();
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    QRectF board(r.left(), r.top()+r.height()*.18, r.width(), r.height()*.82);
    p.drawRoundedRect(board, 2, 2);
    p.setPen(Qt::NoPen); p.setBrush(c);
    p.drawRect(QRectF(r.left(), r.top(), r.width(), r.height()*.18));
    p.setPen(QPen(c, 1.8)); p.setBrush(Qt::NoBrush);
    for (int i = 1; i < 4; ++i) {
        qreal y = board.top() + board.height()*i/4.0;
        p.drawLine(QPointF(board.left()+4,y), QPointF(board.right()-4,y));
    }
    p.restore();
}

static void drawSeriesIcon(QPainter &p, QRectF r, QColor c)
{
    p.save();
    p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    QRectF screen(r.left(), r.top(), r.width(), r.height()*.74);
    p.drawRoundedRect(screen, 3, 3);
    qreal mx = r.center().x();
    p.drawLine(QPointF(mx, screen.bottom()), QPointF(mx, r.bottom()));
    p.drawLine(QPointF(mx-r.width()*.25, r.bottom()),
               QPointF(mx+r.width()*.25, r.bottom()));
    p.restore();
}

static void drawMusicIcon(QPainter &p, QRectF r, QColor c)
{
    p.save();
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    qreal nr = r.width()*.22, nx = r.left()+r.width()*.30;
    qreal ny = r.bottom()-nr-2;
    p.drawEllipse(QPointF(nx,ny), nr, nr*.72);
    qreal sx = nx+nr*.85;
    p.drawLine(QPointF(sx,ny-1), QPointF(sx,r.top()+4));
    p.drawLine(QPointF(sx,r.top()+4), QPointF(r.right()-2,r.top()+12));
    p.restore();
}

static void drawGearIcon(QPainter &p, QRectF r, QColor c)
{
    p.save();
    static const double TAU = 2.0*std::acos(-1.0);
    double cx=r.center().x(), cy=r.center().y();
    double outerR=r.width()*.44, innerR=r.width()*.20;
    const int teeth=7;
    QPainterPath gear;
    for (int i=0; i<teeth*2; ++i) {
        double a  = TAU*i/(teeth*2) - TAU/4;
        double ra = (i%2==0) ? outerR : outerR*.74;
        QPointF pt(cx+ra*std::cos(a), cy+ra*std::sin(a));
        if (i==0) gear.moveTo(pt); else gear.lineTo(pt);
    }
    gear.closeSubpath();
    QPainterPath hole;
    hole.addEllipse(QPointF(cx,cy), innerR, innerR);
    p.setPen(Qt::NoPen); p.setBrush(c);
    p.drawPath(gear.subtracted(hole));
    p.restore();
}

// ─────────────────────────────────────────────────────────────────────────────
//  ContentCard  –  handles Movie, Series, and Music tiles
// ─────────────────────────────────────────────────────────────────────────────
class ContentCard : public QWidget
{
    ContentItem  m_data;
    bool         m_focused = false;
    bool         m_music;
    QPixmap      m_thumb;

    std::function<void(ContentCard *)> m_focusCb;
    std::function<void(ContentCard *)> m_confirmCb;

public:
    ContentCard(const ContentItem &data, QWidget *parent = nullptr)
        : QWidget(parent), m_data(data),
          m_music(data.kind == CardKind::Music)
    {
        int w = m_music ? TV::MUSIC_SZ : TV::CARD_W;
        int h = m_music ? TV::MUSIC_SZ : TV::CARD_H;
        setFixedSize(w, h);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        buildThumb();
    }

    void setFocusCb  (std::function<void(ContentCard *)> cb) { m_focusCb   = cb; }
    void setConfirmCb(std::function<void(ContentCard *)> cb) { m_confirmCb = cb; }
    const ContentItem &item() const { return m_data; }

private:
    // Pre-render the static thumbnail once.
    void buildThumb()
    {
        int w = width(), h = height();
        m_thumb = QPixmap(w, h);
        m_thumb.fill(Qt::transparent);
        QPainter p(&m_thumb);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

        // Rounded clip
        QPainterPath clip;
        clip.addRoundedRect(0, 0, w, h, TV::CARD_R, TV::CARD_R);
        p.setClipPath(clip);

        // Image or gradient placeholder
        bool imgOk = false;
        if (!m_data.imagePath.isEmpty() && QFile::exists(m_data.imagePath)) {
            QPixmap src(m_data.imagePath);
            src = src.scaled(w, h, Qt::KeepAspectRatioByExpanding,
                             Qt::SmoothTransformation);
            p.drawPixmap(-(src.width()-w)/2, -(src.height()-h)/2, src);
            imgOk = true;
        }
        if (!imgOk) {
            int hue = m_data.title.isEmpty() ? 210
                    : qAbs(m_data.title[0].unicode()*53+190)%360;
            QLinearGradient g(0,0,w,h);
            g.setColorAt(0, QColor::fromHsv(hue,          110, 65));
            g.setColorAt(1, QColor::fromHsv((hue+45)%360,  90, 30));
            p.fillRect(0,0,w,h,g);
            p.setFont(QFont("Roboto", m_music ? 54 : 72, QFont::Bold));
            p.setPen(QColor(255,255,255,30));
            p.drawText(QRect(0,-12,w,h), Qt::AlignCenter, m_data.title.left(1));
        }

        // Bottom scrim
        int scrimH = m_music ? 55 : 82;
        QLinearGradient scrim(0, h-scrimH, 0, h);
        scrim.setColorAt(0, QColor(0,0,0,0));
        scrim.setColorAt(1, QColor(0,0,0, m_music ? 195 : 232));
        p.fillRect(0, h-scrimH, w, scrimH, scrim);

        // Title text
        QFont tf("Roboto", m_music ? 11 : 13, QFont::Bold);
        p.setFont(tf);
        p.setPen(TV::TEXT_PRI);
        QString el = QFontMetrics(tf).elidedText(m_data.title,Qt::ElideRight,w-20);
        p.drawText(QRect(10, h-(m_music?28:52), w-20, 22),
                   Qt::AlignLeft | Qt::AlignVCenter, el);

        // Meta line (video cards only)
        if (!m_music) {
            p.setFont(QFont("Roboto", 10));
            p.setPen(TV::TEXT_SEC);
            p.drawText(QRect(10, h-28, w-20, 20),
                       Qt::AlignLeft | Qt::AlignVCenter, m_data.meta);
        }
        p.end();
    }

    // Overlaid on music card when focused: dim + white circle + play triangle
    void paintPlayOverlay(QPainter &p)
    {
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0,0,width(),height()), TV::CARD_R, TV::CARD_R);
        p.setClipPath(clip);
        p.fillRect(rect(), QColor(0,0,0,110));
        p.setClipping(false);

        const int cr = 34;
        QPoint c(width()/2, height()/2 - 8);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255,255,255,220));
        p.drawEllipse(c, cr, cr);

        QPolygon tri;
        tri << QPoint(c.x()-9,  c.y()-13)
            << QPoint(c.x()-9,  c.y()+13)
            << QPoint(c.x()+16, c.y());
        p.setBrush(QColor(20,20,20,235));
        p.drawPolygon(tri);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Multi-layer glow halo
        if (m_focused) {
            for (int i = 7; i >= 1; --i) {
                QPainterPath gp;
                gp.addRoundedRect(QRectF(i,i,width()-2*i,height()-2*i),
                                  TV::CARD_R+2, TV::CARD_R+2);
                p.setPen(QPen(QColor(168,199,250,10*i), 2));
                p.setBrush(Qt::NoBrush);
                p.drawPath(gp);
            }
        }

        p.drawPixmap(0, 0, m_thumb);

        // Music: play button overlay when focused
        if (m_focused && m_music)
            paintPlayOverlay(p);

        // Focus border
        if (m_focused) {
            p.setBrush(Qt::NoBrush);
            QPainterPath bp;
            bp.addRoundedRect(QRectF(1.5,1.5,width()-3,height()-3),
                              TV::CARD_R, TV::CARD_R);
            p.setPen(QPen(TV::FOCUS_CLR, 3));
            p.drawPath(bp);
        }
    }

    void focusInEvent(QFocusEvent *e) override
    {
        m_focused = true;  update();
        if (m_focusCb) m_focusCb(this);
        QWidget::focusInEvent(e);
    }
    void focusOutEvent(QFocusEvent *e) override
    {
        m_focused = false; update();
        QWidget::focusOutEvent(e);
    }

    // Key handling delegated entirely to DpadFilter → MainWindow::handleKey.
    void keyPressEvent(QKeyEvent *e) override { e->ignore(); }

    void mousePressEvent(QMouseEvent *e) override
    {
        setFocus();
        if (m_confirmCb) m_confirmCb(this);
        QWidget::mousePressEvent(e);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  SidebarOverlay  –  left panel, always full-height, shown/hidden on demand
// ─────────────────────────────────────────────────────────────────────────────
class SidebarOverlay : public QWidget
{
    struct NavItem { QString label; Category cat; };
    QVector<NavItem> m_items;
    int              m_sel    = 0;
    Category         m_active = Category::Movies;

    std::function<void(Category)> m_selectCb;
    std::function<void()>         m_closeCb;

    // Layout constants
    enum { ITEM_H=60, ITEM_Y0=210, ITEM_GAP=8,
           ICON_CX=56, ICON_SZ=24, LABEL_X=88 };

public:
    explicit SidebarOverlay(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedWidth(TV::SIDEBAR_W);
        m_items = { {"Search", Category::Search},
                    {"Movies", Category::Movies},
                    {"Series", Category::Series},
                    {"Music",  Category::Music } };
    }

    void setSelectCb(std::function<void(Category)> cb) { m_selectCb = cb; }
    void setCloseCb (std::function<void()>         cb) { m_closeCb  = cb; }

    void setActiveCategory(Category c)
    {
        m_active = c;
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items[i].cat == c) { m_sel = i; break; }
        update();
    }

    void moveUp()   { if (m_sel > 0)               { --m_sel; update(); } }
    void moveDown() { if (m_sel < m_items.size()-1) { ++m_sel; update(); } }
    void confirm()  { if (m_selectCb) m_selectCb(m_items[m_sel].cat); }
    void dismiss()  { if (m_closeCb)  m_closeCb(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

        // Panel background
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(16, 16, 20, 248));
        p.drawRect(rect());

        // ── User avatar + name ────────────────────────────────────────────
        QRectF av(TV::MARGIN_V, 46, 52, 52);
        p.setBrush(QColor(110, 75, 195));
        p.drawEllipse(av);
        p.setFont(QFont("Roboto", 15, QFont::Bold));
        p.setPen(Qt::white);
        p.drawText(av, Qt::AlignCenter, "AM");

        p.setFont(QFont("Roboto", 17, QFont::Bold));
        p.setPen(TV::TEXT_PRI);
        p.drawText(QRect(int(av.right())+14, 48,
                         width()-int(av.right())-18, 26),
                   Qt::AlignLeft|Qt::AlignVCenter, "Ashley Miller");

        p.setFont(QFont("Roboto", 12));
        p.setPen(TV::TEXT_SEC);
        p.drawText(QRect(int(av.right())+14, 76,
                         width()-int(av.right())-18, 22),
                   Qt::AlignLeft|Qt::AlignVCenter, "Switch account");

        // Divider
        p.setPen(QColor(255,255,255,20));
        p.drawLine(24, 130, width()-24, 130);

        // ── Nav items ─────────────────────────────────────────────────────
        for (int i = 0; i < m_items.size(); ++i) {
            bool sel    = (i == m_sel);
            bool active = (m_items[i].cat == m_active);

            QRect pill(18, ITEM_Y0 + i*(ITEM_H+ITEM_GAP),
                       width()-36, ITEM_H);

            // Pill background
            p.setPen(Qt::NoPen);
            if (sel) {
                p.setBrush(QColor(238,238,238));
                p.drawRoundedRect(pill, pill.height()/2, pill.height()/2);
            } else if (active) {
                p.setBrush(QColor(168,199,250,35));
                p.drawRoundedRect(pill, pill.height()/2, pill.height()/2);
            }

            // Icon
            QColor ic = sel ? QColor(30,30,30) : TV::TEXT_PRI;
            QRectF iconR(pill.left()+ICON_CX-ICON_SZ/2,
                         pill.center().y()-ICON_SZ/2,
                         ICON_SZ, ICON_SZ);
            switch (m_items[i].cat) {
            case Category::Search: drawSearchIcon(p, iconR, ic); break;
            case Category::Movies: drawMoviesIcon(p, iconR, ic); break;
            case Category::Series: drawSeriesIcon(p, iconR, ic); break;
            case Category::Music:  drawMusicIcon (p, iconR, ic); break;
            }

            // Label
            p.setPen(ic);
            p.setFont(QFont("Roboto", 16, sel ? QFont::Bold : QFont::Normal));
            p.drawText(QRect(pill.left()+LABEL_X, pill.top(),
                             pill.width()-LABEL_X-8, pill.height()),
                       Qt::AlignLeft|Qt::AlignVCenter, m_items[i].label);
        }

        // ── Settings entry at bottom ──────────────────────────────────────
        int sy  = height() - TV::MARGIN_V - ITEM_H;
        QRect sp(18, sy, width()-36, ITEM_H);
        QRectF gR(sp.left()+ICON_CX-ICON_SZ/2,
                  sp.center().y()-ICON_SZ/2, ICON_SZ, ICON_SZ);
        drawGearIcon(p, gR, TV::TEXT_SEC);
        p.setPen(TV::TEXT_SEC);
        p.setFont(QFont("Roboto", 16));
        p.drawText(QRect(sp.left()+LABEL_X, sp.top(),
                         sp.width()-LABEL_X-8, sp.height()),
                   Qt::AlignLeft|Qt::AlignVCenter, "Settings");
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        for (int i = 0; i < m_items.size(); ++i) {
            QRect pill(18, ITEM_Y0 + i*(ITEM_H+ITEM_GAP), width()-36, ITEM_H);
            if (pill.contains(e->pos())) {
                m_sel = i; update(); confirm(); return;
            }
        }
        QWidget::mousePressEvent(e);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  DetailOverlay  –  full-screen dim + centred content card with action buttons
// ─────────────────────────────────────────────────────────────────────────────
class DetailOverlay : public QWidget
{
    ContentItem m_item;
    QPixmap     m_thumb;
    int         m_btnSel  = 0;
    QRect       m_btnRect[2];

    std::function<void()> m_closeCb;

    enum Dims { DW=520, IMGH=260, BTNH=52, BPAD=24 };

public:
    explicit DetailOverlay(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        hide();
    }

    void setCloseCb(std::function<void()> cb) { m_closeCb = cb; }

    void showItem(const ContentItem &item)
    {
        m_item = item; m_btnSel = 0;
        buildThumb();
        show(); raise(); update();
    }

    void moveUp()   { if (m_btnSel > 0) { --m_btnSel; update(); } }
    void moveDown() { if (m_btnSel < 1) { ++m_btnSel; update(); } }
    void dismiss()  { if (m_closeCb) m_closeCb(); }

    void confirm()
    {
        if (m_btnSel == 0) {
            // Play
            if (!m_item.filePath.isEmpty() && QFile::exists(m_item.filePath))
                QProcess::startDetached("mpv",
                    QStringList() << "--vo=gpu"
                    << "--audio-device=alsa/sysdefault:CARD=hdmi0"
                    << m_item.filePath);
            else
                qDebug() << "No media file for:" << m_item.title;
            if (m_closeCb) m_closeCb();
        } else {
            // Favourites – stub
            qDebug() << "Toggle favourites:" << m_item.title;
        }
    }

private:
    void buildThumb()
    {
        m_thumb = QPixmap(DW, IMGH);
        m_thumb.fill(Qt::transparent);
        QPainter p(&m_thumb);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

        QPainterPath cl;
        cl.addRoundedRect(0, 0, DW, IMGH+20, 16, 16);
        p.setClipPath(cl);

        if (!m_item.imagePath.isEmpty() && QFile::exists(m_item.imagePath)) {
            QPixmap src(m_item.imagePath);
            src = src.scaled(DW, IMGH, Qt::KeepAspectRatioByExpanding,
                             Qt::SmoothTransformation);
            p.drawPixmap(-(src.width()-DW)/2, -(src.height()-IMGH)/2, src);
        } else {
            int hue = m_item.title.isEmpty() ? 200
                    : qAbs(m_item.title[0].unicode()*53+190)%360;
            QLinearGradient g(0,0,DW,IMGH);
            g.setColorAt(0, QColor::fromHsv(hue,          100, 72));
            g.setColorAt(1, QColor::fromHsv((hue+50)%360,  85, 35));
            p.fillRect(0,0,DW,IMGH,g);
            p.setFont(QFont("Roboto",72,QFont::Bold));
            p.setPen(QColor(255,255,255,30));
            p.drawText(QRect(0,-16,DW,IMGH),Qt::AlignCenter,m_item.title.left(1));
        }
        p.end();
    }

    // Total card height – must match paintEvent layout exactly.
    int cardH() const
    {
        int h = IMGH;
        h += 16 + 38;   // gap + title row
        h += 32;        // meta row
        if (!m_item.description.isEmpty()) h += 92;
        h += (BTNH+10) + BTNH + BPAD;   // two buttons + bottom pad
        return h;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Full-screen dim
        p.fillRect(rect(), QColor(0,0,0,158));

        const int dh = cardH();
        const int cx = (width()-DW)/2, cy = (height()-dh)/2;
        QRect card(cx, cy, DW, dh);

        // Drop shadow
        for (int i = 10; i >= 1; --i) {
            QPainterPath sp;
            sp.addRoundedRect(card.adjusted(-i,-i,i,i+2), 18, 18);
            p.fillPath(sp, QColor(0,0,0,7));
        }

        // Card body
        QPainterPath cardPath;
        cardPath.addRoundedRect(card, 16, 16);
        p.fillPath(cardPath, TV::SURFACE);

        // Image (clipped to card top corners)
        p.setClipPath(cardPath);
        p.drawPixmap(cx, cy, m_thumb);
        p.setClipping(false);

        // Soft fade from image into card surface
        QLinearGradient fade(0, cy+IMGH-60, 0, cy+IMGH);
        fade.setColorAt(0, QColor(TV::SURFACE.red(),TV::SURFACE.green(),
                                  TV::SURFACE.blue(), 0));
        fade.setColorAt(1, TV::SURFACE);
        p.fillRect(cx, cy+IMGH-60, DW, 60, fade);

        int ty = cy + IMGH + 16;

        // Title
        p.setFont(QFont("Roboto", 21, QFont::Bold));
        p.setPen(TV::TEXT_PRI);
        p.drawText(QRect(cx+BPAD, ty, DW-BPAD*2, 32),
                   Qt::AlignLeft|Qt::AlignVCenter, m_item.title);
        ty += 38;

        // Meta
        p.setFont(QFont("Roboto", 13));
        p.setPen(TV::TEXT_SEC);
        p.drawText(QRect(cx+BPAD, ty, DW-BPAD*2, 22),
                   Qt::AlignLeft|Qt::AlignVCenter, m_item.meta);
        ty += 32;

        // Description
        if (!m_item.description.isEmpty()) {
            p.setFont(QFont("Roboto", 13));
            p.setPen(QColor(185,185,185));
            p.drawText(QRect(cx+BPAD, ty, DW-BPAD*2, 80),
                       Qt::AlignLeft|Qt::AlignTop|Qt::TextWordWrap,
                       m_item.description);
            ty += 92;
        }

        // ── Buttons ───────────────────────────────────────────────────────
        auto drawBtn = [&](int idx, const QString &label, bool primary) {
            bool sel = (m_btnSel == idx);
            QRect r(cx+BPAD, ty, DW-BPAD*2, BTNH);
            m_btnRect[idx] = r;

            p.setPen(Qt::NoPen);
            if (primary)
                p.setBrush(sel ? QColor(215,215,215) : QColor(245,245,245));
            else
                p.setBrush(sel ? QColor(55,55,65) : QColor(36,36,46));
            p.drawRoundedRect(r, BTNH/2, BTNH/2);

            // Secondary outline
            if (!primary) {
                p.setPen(QPen(QColor(85,85,96), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(1,1,-1,-1), BTNH/2, BTNH/2);
                p.setPen(Qt::NoPen);
            }

            // Play triangle on primary button
            if (primary) {
                QPolygon tri;
                int ax = r.center().x()-44, ay = r.center().y();
                tri << QPoint(ax,ay-8) << QPoint(ax,ay+8) << QPoint(ax+14,ay);
                p.setBrush(QColor(20,20,20));
                p.drawPolygon(tri);
            }

            p.setFont(QFont("Roboto", 15, QFont::Medium));
            p.setPen(primary ? QColor(15,15,15) : TV::TEXT_PRI);
            p.drawText(r, Qt::AlignCenter, label);

            // Focus ring
            if (sel) {
                p.setPen(QPen(TV::FOCUS_CLR, 2));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(-2,-2,2,2), BTNH/2+2, BTNH/2+2);
                p.setPen(Qt::NoPen);
            }
            ty += BTNH + 10;
        };

        drawBtn(0, "  Play", true);
        drawBtn(1, "Add to Favorites", false);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        const int dh = cardH();
        QRect card((width()-DW)/2, (height()-dh)/2, DW, dh);
        if (!card.contains(e->pos())) { dismiss(); return; }
        for (int i = 0; i < 2; ++i)
            if (m_btnRect[i].contains(e->pos()))
                { m_btnSel = i; confirm(); return; }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  CardRow / MainWindow
// ─────────────────────────────────────────────────────────────────────────────
struct CardRow {
    QVector<ContentCard *> cards;
    QScrollArea           *hScroll = nullptr;
};

// Forward declaration required by GamepadReader.
class MainWindow;

class MainWindow : public QWidget
{
    AppState         m_state    = AppState::Browse;
    Category         m_category = Category::Movies;

    QVector<CardRow> m_rows;
    int              m_r = 0, m_c = 0;

    QLabel          *m_header    = nullptr;
    QLabel          *m_infoTitle = nullptr;
    QLabel          *m_infoMeta  = nullptr;
    QScrollArea     *m_vScroll   = nullptr;
    SidebarOverlay  *m_sidebar   = nullptr;
    DetailOverlay   *m_detail    = nullptr;

    QGraphicsOpacityEffect *m_sidebarOpacity = nullptr;
    QPropertyAnimation     *m_sidebarSlide   = nullptr;
    QPropertyAnimation     *m_sidebarFade    = nullptr;
    QParallelAnimationGroup *m_sidebarAnim   = nullptr;

    bool m_switchingCategory = false;
    bool m_sidebarClosing = false;

public:
    explicit MainWindow(QWidget *parent = nullptr) : QWidget(parent)
    {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setStyleSheet(QString("QWidget { background-color: %1; }")
                      .arg(TV::BG.name()));

        // Create overlays first (resizeEvent may fire during buildShell)
        m_sidebar = new SidebarOverlay(this);
        m_sidebar->hide();
        m_sidebarOpacity = new QGraphicsOpacityEffect(m_sidebar);
        m_sidebarOpacity->setOpacity(1.0);
        m_sidebar->setGraphicsEffect(m_sidebarOpacity);

        m_sidebarSlide = new QPropertyAnimation(m_sidebar, "pos", this);
        m_sidebarFade  = new QPropertyAnimation(m_sidebarOpacity, "opacity", this);
        m_sidebarAnim  = new QParallelAnimationGroup(this);
        m_sidebarAnim->addAnimation(m_sidebarSlide);
        m_sidebarAnim->addAnimation(m_sidebarFade);
        QObject::connect(m_sidebarAnim, &QParallelAnimationGroup::finished,
                         this, [this]() {
            if (m_sidebarClosing) {
                m_sidebarClosing = false;
                if (m_state == AppState::Sidebar) {
                    m_state = AppState::Browse;
                    m_sidebar->hide();
                    restoreFocus();
                }
            }
        });
        m_sidebar->setSelectCb([this](Category c){ switchCategory(c); });
        m_sidebar->setCloseCb ([this](){ closeSidebar(); });

        m_detail = new DetailOverlay(this);
        m_detail->hide();
        m_detail->setCloseCb([this](){ closeDetail(); });

        buildShell();
        switchCategory(Category::Movies);
    }

    void setInitialFocus()
    {
        if (!m_rows.isEmpty() && !m_rows[0].cards.isEmpty())
            m_rows[0].cards[0]->setFocus();
    }

    AppState appState() const { return m_state; }

    // ── Overlay lifecycle ─────────────────────────────────────────────────
    void openSidebar()
    {
        if (m_state != AppState::Browse || m_switchingCategory) return;
        m_state = AppState::Sidebar;
        m_sidebar->setActiveCategory(m_category);

        if (m_sidebarAnim->state() == QAbstractAnimation::Running)
            m_sidebarAnim->stop();

        const int y = 0;
        m_sidebar->move(-m_sidebar->width(), y);
        m_sidebarOpacity->setOpacity(0.0);
        m_sidebar->show();
        m_sidebar->raise();

        m_sidebarSlide->setDuration(220);
        m_sidebarSlide->setEasingCurve(QEasingCurve::OutCubic);
        m_sidebarSlide->setStartValue(QPoint(-m_sidebar->width(), y));
        m_sidebarSlide->setEndValue(QPoint(0, y));

        m_sidebarFade->setDuration(220);
        m_sidebarFade->setEasingCurve(QEasingCurve::OutCubic);
        m_sidebarFade->setStartValue(0.0);
        m_sidebarFade->setEndValue(1.0);

        m_sidebarAnim->start();
    }

    void closeSidebar()
    {
        if (m_state != AppState::Sidebar) return;

        if (m_sidebarAnim->state() == QAbstractAnimation::Running)
            m_sidebarAnim->stop();

        const int y = 0;
        m_sidebarSlide->setDuration(180);
        m_sidebarSlide->setEasingCurve(QEasingCurve::InCubic);
        m_sidebarSlide->setStartValue(m_sidebar->pos());
        m_sidebarSlide->setEndValue(QPoint(-m_sidebar->width(), y));

        m_sidebarFade->setDuration(180);
        m_sidebarFade->setEasingCurve(QEasingCurve::InCubic);
        m_sidebarFade->setStartValue(m_sidebarOpacity->opacity());
        m_sidebarFade->setEndValue(0.0);

        m_sidebarClosing = true;
        m_sidebarAnim->start();
    }

    void openDetail(const ContentItem &item)
    {
        m_state = AppState::Detail;
        m_detail->showItem(item);
    }

    void closeDetail()
    {
        m_state = AppState::Browse;
        m_detail->hide();
        restoreFocus();
    }

    // ── Central key-dispatch (called by DpadFilter and GamepadReader) ─────
    void handleKey(int key)
    {
        switch (m_state) {
        case AppState::Sidebar:
            switch (key) {
            case Qt::Key_Up:                         m_sidebar->moveUp();   break;
            case Qt::Key_Down:                       m_sidebar->moveDown(); break;
            case Qt::Key_Return: case Qt::Key_Enter: m_sidebar->confirm();  break;
            case Qt::Key_Escape: case Qt::Key_Left:  closeSidebar();        break;
            default: break;
            }
            break;

        case AppState::Detail:
            switch (key) {
            case Qt::Key_Up:                         m_detail->moveUp();   break;
            case Qt::Key_Down:                       m_detail->moveDown(); break;
            case Qt::Key_Return: case Qt::Key_Enter: m_detail->confirm();  break;
            case Qt::Key_Escape:                     closeDetail();        break;
            default: break;
            }
            break;

        case AppState::Browse:
            switch (key) {
            case Qt::Key_Left:   navigate( 0,-1); break;
            case Qt::Key_Right:  navigate( 0, 1); break;
            case Qt::Key_Up:     navigate(-1, 0); break;
            case Qt::Key_Down:   navigate( 1, 0); break;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (!m_rows.isEmpty() && m_r < m_rows.size()
                        && m_c < m_rows[m_r].cards.size())
                    onCardConfirmed(m_rows[m_r].cards[m_c]);
                break;
            case Qt::Key_Escape: close(); break;
            default: break;
            }
            break;
        }
    }

    // ── Called by GamepadReader (A button) ────────────────────────────────
    void activateFocused() { handleKey(Qt::Key_Return); }

    void navigate(int dr, int dc)
    {
        if (m_rows.isEmpty()) return;
        int nr = qBound(0, m_r+dr, m_rows.size()-1);
        int nc = (dr != 0) ? qBound(0, m_c, m_rows[nr].cards.size()-1)
                           : qBound(0, m_c+dc, m_rows[nr].cards.size()-1);
        m_r = nr; m_c = nc;

        ContentCard *card = m_rows[m_r].cards[m_c];
        card->setFocus();
        if (m_rows[m_r].hScroll)
            m_rows[m_r].hScroll->ensureWidgetVisible(card, TV::GUTTER, 0);
        if (m_vScroll)
            m_vScroll->ensureWidgetVisible(card, 0, TV::ROW_GAP);
    }

    void onCardFocused(ContentCard *card)
    {
        if (m_infoTitle) m_infoTitle->setText(card->item().title);
        if (m_infoMeta)  m_infoMeta->setText(card->item().meta);
        for (int r=0; r<m_rows.size(); ++r)
            for (int c=0; c<m_rows[r].cards.size(); ++c)
                if (m_rows[r].cards[c] == card) { m_r=r; m_c=c; return; }
    }

    void onCardConfirmed(ContentCard *card)
    {
        const ContentItem &item = card->item();
        if (item.kind == CardKind::Music) {
            // Music: play immediately, no detail overlay.
            if (!item.filePath.isEmpty() && QFile::exists(item.filePath))
                QProcess::startDetached("mpv",
                    QStringList() << "--vo=gpu"
                    << "--audio-device=alsa/sysdefault:CARD=hdmi0"
                    << item.filePath);
            else
                qDebug() << "No audio file:" << item.title;
        } else {
            // Movie / Series: open the detail overlay.
            openDetail(item);
        }
    }

private:
    void restoreFocus()
    {
        if (m_rows.isEmpty()) return;
        m_r = qBound(0, m_r, m_rows.size()-1);
        m_c = qBound(0, m_c, m_rows[m_r].cards.size()-1);
        if (!m_rows[m_r].cards.isEmpty())
            m_rows[m_r].cards[m_c]->setFocus();
    }

    // ── Build fixed shell (header, info banner, scroll area) ─────────────
    void buildShell()
    {
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(TV::MARGIN_H, TV::MARGIN_V,
                                 TV::MARGIN_H, TV::MARGIN_V);
        root->setSpacing(0);

        // Header row
        auto *hdr = new QWidget; hdr->setStyleSheet("background:transparent;");
        hdr->setFixedHeight(52);
        auto *hl = new QHBoxLayout(hdr);
        hl->setContentsMargins(0,0,0,0);

        m_header = new QLabel;
        m_header->setStyleSheet(QString(
            "color:%1; font:bold 26px 'Roboto';"
            " letter-spacing:4px; background:transparent;")
            .arg(TV::TEXT_PRI.name()));
        hl->addWidget(m_header);
        hl->addStretch();

        // Hint text (ASCII-safe, no Unicode arrows in this slot)
        auto *hint = new QLabel(
            "[Home] menu   [Arr] navigate   [Enter] select/play   [Esc] quit");
        hint->setStyleSheet("color:#3A3A3A; font:13px 'Roboto';"
                            " background:transparent;");
        hl->addWidget(hint);
        root->addWidget(hdr);
        root->addSpacing(12);

        // Info banner (shows currently focused item)
        auto *banner = new QWidget; banner->setStyleSheet("background:transparent;");
        banner->setFixedHeight(36);
        auto *bl = new QHBoxLayout(banner);
        bl->setContentsMargins(0,0,0,0); bl->setSpacing(14);
        m_infoTitle = new QLabel("\u2014");
        m_infoTitle->setStyleSheet(QString(
            "color:%1; font:bold 17px 'Roboto'; background:transparent;")
            .arg(TV::TEXT_PRI.name()));
        m_infoMeta = new QLabel;
        m_infoMeta->setStyleSheet(QString(
            "color:%1; font:13px 'Roboto'; background:transparent;")
            .arg(TV::TEXT_SEC.name()));
        bl->addWidget(m_infoTitle); bl->addWidget(m_infoMeta); bl->addStretch();
        root->addWidget(banner);
        root->addSpacing(18);

        // Vertical scroll area (content swapped per category)
        m_vScroll = new QScrollArea;
        m_vScroll->setFrameShape(QFrame::NoFrame);
        m_vScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_vScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_vScroll->setStyleSheet("background:transparent;");
        m_vScroll->setWidgetResizable(true);
        root->addWidget(m_vScroll);
    }

    // ── Rebuild content rows for the chosen category ──────────────────────
    void switchCategory(Category cat)
    {
        if (m_switchingCategory) return;
        m_switchingCategory = true;

        m_category = cat;
        m_rows.clear();
        m_r = 0; m_c = 0;

        static const char *labels[] = {"SEARCH","MOVIES","SERIES","MUSIC"};
        m_header->setText(labels[static_cast<int>(cat)]);

        auto *vc = new QWidget; vc->setStyleSheet("background:transparent;");
        auto *vl = new QVBoxLayout(vc);
        vl->setContentsMargins(0,0,0,0); vl->setSpacing(TV::ROW_GAP);

        switch (cat) {
        case Category::Search: buildSearch(vl); break;
        case Category::Movies: buildMovies(vl); break;
        case Category::Series: buildSeries(vl); break;
        case Category::Music:  buildMusic (vl); break;
        }
        vl->addStretch();

        QWidget *oldContent = m_vScroll->widget();
        if (oldContent) {
            auto *oldFx = new QGraphicsOpacityEffect(oldContent);
            oldContent->setGraphicsEffect(oldFx);
            auto *oldFade = new QPropertyAnimation(oldFx, "opacity", oldContent);
            oldFade->setDuration(150);
            oldFade->setEasingCurve(QEasingCurve::InOutQuad);
            oldFade->setStartValue(1.0);
            oldFade->setEndValue(0.0);
            QObject::connect(oldFade, &QPropertyAnimation::finished,
                             oldContent, &QWidget::deleteLater);
            oldFade->start(QAbstractAnimation::DeleteWhenStopped);
        }

        m_vScroll->takeWidget();
        m_vScroll->setWidget(vc);

        auto *fx = new QGraphicsOpacityEffect(vc);
        vc->setGraphicsEffect(fx);
        fx->setOpacity(0.0);
        vc->move(36, 0);

        auto *slide = new QPropertyAnimation(vc, "pos", vc);
        slide->setDuration(230);
        slide->setEasingCurve(QEasingCurve::OutCubic);
        slide->setStartValue(QPoint(36, 0));
        slide->setEndValue(QPoint(0, 0));

        auto *fade = new QPropertyAnimation(fx, "opacity", vc);
        fade->setDuration(230);
        fade->setEasingCurve(QEasingCurve::OutCubic);
        fade->setStartValue(0.0);
        fade->setEndValue(1.0);

        auto *group = new QParallelAnimationGroup(vc);
        group->addAnimation(slide);
        group->addAnimation(fade);

        QObject::connect(group, &QParallelAnimationGroup::finished, this, [this, vc]() {
            vc->move(0, 0);
            vc->setGraphicsEffect(nullptr);
            setInitialFocus();
            m_switchingCategory = false;
            m_state = AppState::Browse;
            m_sidebar->hide();
        });

        if (m_sidebarAnim->state() == QAbstractAnimation::Running)
            m_sidebarAnim->stop();
        m_sidebar->hide();

        group->start(QAbstractAnimation::DeleteWhenStopped);
    }

    // ── Content builders ──────────────────────────────────────────────────
    void buildSearch(QVBoxLayout *vl)
    {
        // NOTE: DpadFilter does NOT intercept regular character keys, so
        // typing works normally in the QLineEdit.  Arrow keys for cursor
        // movement inside the field ARE intercepted; this is an acceptable
        // trade-off for a TV remote interface.
        auto *bar = new QLineEdit;
        bar->setPlaceholderText("Search movies, series, music\u2026");
        bar->setStyleSheet(QString(
            "QLineEdit{ background:#1E1E1E; color:%1;"
            " border:1px solid #333; border-radius:8px;"
            " padding:10px 16px; font:16px 'Roboto'; }"
            "QLineEdit:focus{ border-color:%2; }")
            .arg(TV::TEXT_PRI.name(), TV::FOCUS_CLR.name()));
        bar->setFixedHeight(50);
        vl->addWidget(bar);
        vl->addSpacing(16);
        auto *ph = new QLabel("Type to search\u2026");
        ph->setStyleSheet(QString("color:%1; font:16px 'Roboto';"
                                  " background:transparent;")
                          .arg(TV::TEXT_SEC.name()));
        ph->setAlignment(Qt::AlignCenter);
        vl->addWidget(ph);
    }

    void buildMovies(QVBoxLayout *vl)
    {
        using CI = ContentItem;
        QVector<CI> lib = {
            {CardKind::Movie,"galactic_quest.png","GALACTIC QUEST",
             "2024 \u00b7 2h 18m \u00b7 Sci-Fi",
             "A rogue crew races across the galaxy to prevent an ancient "
             "weapon from falling into the wrong hands.",
             "/media/big_buck_bunny_1080p_surround.avi"},
            {CardKind::Movie,"","JOURNEY TO MARS",
             "2023 \u00b7 1h 54m \u00b7 Drama",
             "An astronaut\u2019s solo mission to Mars forces her to confront "
             "isolation, fear, and what it means to be human.",""},
            {CardKind::Movie,"","THE VOID",
             "2024 \u00b7 52m \u00b7 Documentary",
             "A deep-dive into the mysteries of black holes and the scientists "
             "dedicated to understanding them.",""},
            {CardKind::Movie,"","STELLAR DRIFT",
             "2022 \u00b7 1h 42m \u00b7 Thriller",
             "When a satellite goes dark, one engineer discovers a conspiracy "
             "that reaches the highest levels of government.",""},
            {CardKind::Movie,"","NOVA RISING",
             "2022 \u00b7 1h 28m \u00b7 Action",
             "An unlikely hero must master ancient powers to stop a rising "
             "empire from plunging the world into chaos.",""},
        };
        addRow(vl, "My Library", lib);

        QVector<CI> recent = {
            {CardKind::Movie,"","DEEP HORIZON",
             "2024 \u00b7 1h 8m \u00b7 Drama",
             "Beneath three miles of ocean, researchers discover something "
             "that was never meant to be found.",""},
            {CardKind::Movie,"","ECLIPSE",
             "2023 \u00b7 24m \u00b7 Short Film",
             "A meditation on light, shadow, and the moments between.",""},
            {CardKind::Movie,"","IRON MERIDIAN",
             "2024 \u00b7 2h 2m \u00b7 Action",
             "An ex-special-forces operative goes rogue to dismantle a weapons "
             "network spanning three continents.",""},
            {CardKind::Movie,"","BLUE FREQUENCY",
             "2023 \u00b7 1h 30m \u00b7 Sci-Fi",
             "A radio engineer picks up a signal from 1977 \u2014 and the "
             "voice on the other end knows her name.",""},
        };
        addRow(vl, "Recently Added", recent);
    }

    void buildSeries(QVBoxLayout *vl)
    {
        using CI = ContentItem;
        QVector<CI> top = {
            {CardKind::Series,"","STELLAR DRIFT",
             "S1 \u00b7 8 Episodes \u00b7 Action",
             "A smuggler and a soldier forge an unlikely alliance against "
             "a collapsing interstellar empire.",""},
            {CardKind::Series,"","DARK ARCHIVE",
             "S2 \u00b7 10 Episodes \u00b7 Thriller",
             "An archivist stumbles on classified files that rewrite everything "
             "she thought she knew about her city.",""},
            {CardKind::Series,"","PERIPHERY",
             "S1 \u00b7 6 Episodes \u00b7 Drama",
             "Five strangers connected by a single event must decide how far "
             "they\u2019ll go to protect their secrets.",""},
            {CardKind::Series,"","ECHO CHAMBER",
             "S3 \u00b7 12 Episodes \u00b7 Sci-Fi",
             "A whistleblower wakes in a simulation \u2014 and suspects "
             "she\u2019s not the only one trapped inside.",""},
        };
        addRow(vl, "Top Series", top);

        QVector<CI> cont = {
            {CardKind::Series,"","THE LONG WINTER",
             "S2 \u00b7 Ep.4 \u00b7 Drama",
             "A family\u2019s survival saga during the most brutal winter "
             "on record.",""},
            {CardKind::Series,"","WIRE FRAME",
             "S1 \u00b7 Ep.2 \u00b7 Comedy",
             "A tech startup\u2019s chaotic rise told through standups and "
             "spectacular failures.",""},
            {CardKind::Series,"","IRON SEAS",
             "S1 \u00b7 Ep.7 \u00b7 Adventure",
             "Future pirates navigate treacherous electric seas in "
             "solar-powered ships.",""},
        };
        addRow(vl, "Continue Watching", cont);
    }

    void buildMusic(QVBoxLayout *vl)
    {
        using CI = ContentItem;
        QVector<CI> albums = {
            {CardKind::Music,"","NEON PULSE",     "Electronic \u00b7 42 min","",""},
            {CardKind::Music,"","SOLAR WINDS",    "Ambient \u00b7 58 min",   "",""},
            {CardKind::Music,"","IRON SKY",       "Rock \u00b7 37 min",      "",""},
            {CardKind::Music,"","DEEP BLUE",      "Jazz \u00b7 48 min",      "",""},
            {CardKind::Music,"","NORTHERN LIGHT", "Classical \u00b7 63 min", "",""},
            {CardKind::Music,"","RED SHIFT",      "Hip-Hop \u00b7 34 min",   "",""},
        };
        addRow(vl, "Albums", albums);

        QVector<CI> recent = {
            {CardKind::Music,"","ECLIPSE (SINGLE)","Electronic \u00b7 4 min","",""},
            {CardKind::Music,"","GRAVITY WELL",    "Ambient \u00b7 7 min",   "",""},
            {CardKind::Music,"","STILL LIFE",      "Classical \u00b7 9 min", "",""},
            {CardKind::Music,"","MIDNIGHT RUN",    "Jazz \u00b7 5 min",      "",""},
        };
        addRow(vl, "Recently Played", recent);
    }

    // ── Build one horizontal card row ─────────────────────────────────────
    void addRow(QVBoxLayout *parent, const QString &title,
                const QVector<ContentItem> &items)
    {
        CardRow row;

        auto *rowW = new QWidget; rowW->setStyleSheet("background:transparent;");
        auto *rl   = new QVBoxLayout(rowW);
        rl->setContentsMargins(0,0,0,0); rl->setSpacing(10);

        // Row heading
        auto *lbl = new QLabel(title);
        lbl->setStyleSheet(QString(
            "color:%1; font:bold 14px 'Roboto';"
            " letter-spacing:1px; background:transparent;")
            .arg(TV::TEXT_PRI.name()));
        rl->addWidget(lbl);

        // Horizontal scroll area
        auto *hs = new QScrollArea;
        hs->setFrameShape(QFrame::NoFrame);
        hs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        hs->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        hs->setStyleSheet("background:transparent;");
        bool music = !items.isEmpty() && items[0].kind == CardKind::Music;
        hs->setFixedHeight((music ? TV::MUSIC_SZ : TV::CARD_H) + 18);
        row.hScroll = hs;

        auto *strip = new QWidget; strip->setStyleSheet("background:transparent;");
        auto *cl    = new QHBoxLayout(strip);
        cl->setContentsMargins(4,9,4,9); cl->setSpacing(TV::GUTTER);

        for (const ContentItem &item : items) {
            auto *card = new ContentCard(item);
            card->setFocusCb  ([this](ContentCard *c){ onCardFocused(c);   });
            card->setConfirmCb([this](ContentCard *c){ onCardConfirmed(c); });
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
    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        // Keep overlays pinned to window edges.
        if (m_sidebar) m_sidebar->setGeometry(0, 0, TV::SIDEBAR_W, height());
        if (m_detail)  m_detail->setGeometry(0, 0, width(), height());
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape) close();
        else QWidget::keyPressEvent(e);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  DpadFilter  –  intercepts navigation keys app-wide and routes via state
// ─────────────────────────────────────────────────────────────────────────────
class DpadFilter : public QObject
{
    MainWindow *m_win;
public:
    explicit DpadFilter(MainWindow *win, QObject *parent = nullptr)
        : QObject(parent), m_win(win) {}

    bool eventFilter(QObject *, QEvent *ev) override
    {
        if (ev->type() != QEvent::KeyPress) return false;
        auto *ke = static_cast<QKeyEvent *>(ev);

        // BTN_START (Linux evdev keycode 314) or Qt::Key_Home → sidebar toggle
        const bool isStart = (ke->key() == Qt::Key_Home)
                          || (ke->nativeScanCode() == 314u);
        if (isStart) {
            if      (m_win->appState() == AppState::Browse)  m_win->openSidebar();
            else if (m_win->appState() == AppState::Sidebar) m_win->closeSidebar();
            return true;
        }

        // Route all navigation & confirm/back keys through the state machine.
        switch (ke->key()) {
        case Qt::Key_Left:   case Qt::Key_Right:
        case Qt::Key_Up:     case Qt::Key_Down:
        case Qt::Key_Return: case Qt::Key_Enter:
        case Qt::Key_Escape: case Qt::Key_Back:
            m_win->handleKey(ke->key());
            return true;    // consumed – prevents scroll areas from eating arrows
        default:
            return false;   // let other keys (typing) reach their target widget
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  GamepadReader  –  evdev Xbox controller  (unchanged logic from v1)
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
            return;
        }
        qDebug() << "GamepadReader: opened" << devPath;
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated,
                this, &GamepadReader::onData);
    }

    ~GamepadReader() override { if (m_fd >= 0) ::close(m_fd); }

private slots:
    void onData()
    {
        struct input_event ev;
        while (::read(m_fd, &ev, sizeof(ev)) ==
               static_cast<ssize_t>(sizeof(ev))) {
            if (ev.type == EV_ABS) {
                if      (ev.code == ABS_HAT0X && ev.value == -1) m_win->handleKey(Qt::Key_Left);
                else if (ev.code == ABS_HAT0X && ev.value ==  1) m_win->handleKey(Qt::Key_Right);
                else if (ev.code == ABS_HAT0Y && ev.value == -1) m_win->handleKey(Qt::Key_Up);
                else if (ev.code == ABS_HAT0Y && ev.value ==  1) m_win->handleKey(Qt::Key_Down);
            } else if (ev.type == EV_KEY && ev.value == 1) {
                switch (ev.code) {
                case BTN_SOUTH:  m_win->activateFocused(); break;  // A → play
                case BTN_EAST:   m_win->handleKey(Qt::Key_Escape); break; // B → back
                case BTN_START:                                    // Start → sidebar
                    if (m_win->appState() == AppState::Browse)
                        m_win->openSidebar();
                    else if (m_win->appState() == AppState::Sidebar)
                        m_win->closeSidebar();
                    break;
                default: break;
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

    // Load bundled Roboto from Qt resources.
    const QStringList fontFiles = {
        ":/fonts/Roboto-Light.ttf",
        ":/fonts/Roboto-Regular.ttf",
        ":/fonts/Roboto-Medium.ttf",
        ":/fonts/Roboto-Bold.ttf"
    };
    for (const QString &f : fontFiles) {
        if (QFontDatabase::addApplicationFont(f) == -1)
            qWarning() << "Failed to load bundled font:" << f;
    }
    app.setFont(QFont("Roboto", 13));

    MainWindow win;

    // Keyboard / TV-remote arrow-key filter
    DpadFilter kbDpad(&win);
    app.installEventFilter(&kbDpad);

    // Xbox controller via evdev.
    // Pass alternate path on command line if the device index differs:
    //   ./myqtapp /dev/input/event3
    const QString gamepadPath = (argc > 1)
        ? QString::fromLocal8Bit(argv[1])
        : QStringLiteral("/dev/input/event2");
    GamepadReader gamepad(gamepadPath, &win);

    win.showFullScreen();
    win.setInitialFocus();

    return app.exec();
}

// Required because GamepadReader carries Q_OBJECT and lives in a .cpp file.
#include "myQtApp.moc"