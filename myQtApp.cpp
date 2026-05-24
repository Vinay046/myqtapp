/*
 * myQtApp.cpp  –  Android TV Browse UI  v3
 *
 * What's new in v3:
 *  1. Home page  (default landing screen)
 *       - Full-width Hero card (featured item) with gradient overlay,
 *         title, description and "Enter to Play" hint
 *       - "Frequently Played" and "Recently Watched" horizontal rows
 *       - Sidebar now includes a "Home" entry at the top
 *  2. B button / Key_Back / Key_Escape no longer closes the app
 *       - In Browse mode on any non-Home page  → navigate back to Home
 *       - In Browse mode already on Home       → no-op (app stays open)
 *       - In Detail overlay                    → dismiss overlay, stay on page
 *       - In Sidebar                           → close sidebar
 *       - App can only be terminated by the OS / system integrator
 *  3. App launches directly to Home (Category::Home)
 *  4. Xbox controller auto-detection via EVIOCGNAME ioctl
 *       (see v2 notes – unchanged)
 *
 * Controller mapping (evdev / xpad kernel module):
 *  BTN_START (code 314)  → open / close sidebar   (also Qt::Key_Home)
 *  ABS_HAT0X -1/+1       → left / right
 *  ABS_HAT0Y -1/+1       → up / down
 *  BTN_SOUTH (A button)  → confirm / play
 *  BTN_EAST  (B button)  → back to Home  (never closes app)
 *
 * Cross-compile / Bitbake notes:
 *  - QT += widgets
 *  - CONFIG += c++11
 *  - RESOURCES += fonts.qrc
 *  - linux/input.h + sys/ioctl.h  from linux-libc-headers in any Yocto sysroot
 *  - No extra LIBS or INCLUDEPATH needed
 *  - No Q_OBJECT on custom classes (std::function replaces signals/slots)
 *    except GamepadReader which retains Q_OBJECT for QSocketNotifier slot
 *
 * Udev rule for non-root access (add to /etc/udev/rules.d/99-gamepad.rules):
 *   SUBSYSTEM=="input", ATTRS{name}=="*Xbox*", MODE="0660", GROUP="input"
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
#include <QDir>
#include <QStringList>
#include <functional>
#include <cmath>

#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
//  Xbox controller auto-detection
//
//  Scans every /dev/input/event* node and returns the path of the first one
//  whose EVIOCGNAME string contains "Xbox" (case-insensitive).
//  Returns an empty QString if no matching device is found.
// ─────────────────────────────────────────────────────────────────────────────
static QString findXboxEventDevice()
{
    QDir inputDir("/dev/input");
    QStringList nodes = inputDir.entryList(
        QStringList() << "event*",
        QDir::System,
        QDir::Name
    );

    for (const QString &node : nodes) {
        QString path = "/dev/input/" + node;
        int fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        char buf[256] = {};
        int rc = ::ioctl(fd, EVIOCGNAME(sizeof(buf) - 1), buf);
        ::close(fd);
        if (rc < 0) continue;

        QString name = QString::fromLocal8Bit(buf);
        qDebug() << " " << path << "→" << name;

        if (name.contains("Xbox", Qt::CaseInsensitive)) {
            qDebug() << "GamepadReader: selected" << path << "(" << name << ")";
            return path;
        }
    }

    qWarning() << "GamepadReader: no Xbox device found under /dev/input/";
    return QString();
}

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
    const int CARD_H    = 213;   // 16:9
    const int CARD_R    =  10;
    const int GUTTER    =  20;
    const int ROW_GAP   =  40;

    const int MUSIC_SZ  = 220;   // square music tile
    const int SIDEBAR_W = 380;

    const int HERO_H    = 420;   // home-page hero banner height
    const int HERO_R    =  14;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Enums & data structs
// ─────────────────────────────────────────────────────────────────────────────
enum class AppState { Browse, Sidebar, Detail };

// Home is the first entry – app launches here; B always returns here.
enum class Category { Home, Search, Movies, Series, Music };

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
//  Icon painters
// ─────────────────────────────────────────────────────────────────────────────
static void drawHomeIcon(QPainter &p, QRectF r, QColor c)
{
    p.save();
    p.setPen(QPen(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    // Roof triangle
    qreal mx = r.center().x();
    QPolygonF roof;
    roof << QPointF(mx, r.top()+2)
         << QPointF(r.right()-1, r.top()+r.height()*.44)
         << QPointF(r.left()+1,  r.top()+r.height()*.44);
    p.drawPolyline(roof);
    // House body
    QRectF body(r.left()+r.width()*.15, r.top()+r.height()*.42,
                r.width()*.70, r.height()*.54);
    p.drawRect(body);
    // Door
    QRectF door(mx-r.width()*.12, body.bottom()-r.height()*.28,
                r.width()*.24, r.height()*.28);
    p.drawRect(door);
    p.restore();
}

static void drawSearchIcon(QPainter &p, QRectF r, QColor c)
{
    p.save();
    p.setPen(QPen(c, 2.2, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    qreal cx = r.left()+r.width()*.40, cy = r.top()+r.height()*.40;
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
    p.drawLine(QPointF(mx,screen.bottom()), QPointF(mx,r.bottom()));
    p.drawLine(QPointF(mx-r.width()*.25,r.bottom()),
               QPointF(mx+r.width()*.25,r.bottom()));
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
//  HeroCard  –  full-width featured banner on the Home page
//
//  Displays a large 16:9 image (or gradient placeholder) with a left-side
//  text panel: title, meta line, description, and a subtle play hint.
//  Focusable and confirmable just like ContentCard.
// ─────────────────────────────────────────────────────────────────────────────
class HeroCard : public QWidget
{
    ContentItem m_data;
    bool        m_focused = false;
    QPixmap     m_bg;

    std::function<void(HeroCard *)> m_focusCb;
    std::function<void(HeroCard *)> m_confirmCb;

public:
    HeroCard(const ContentItem &data, QWidget *parent = nullptr)
        : QWidget(parent), m_data(data)
    {
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        // Width is set by the parent layout; height is fixed.
        setFixedHeight(TV::HERO_H);
    }

    void setFocusCb  (std::function<void(HeroCard *)> cb) { m_focusCb   = cb; }
    void setConfirmCb(std::function<void(HeroCard *)> cb) { m_confirmCb = cb; }
    const ContentItem &item() const { return m_data; }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        buildBg();   // re-render when width is known / changes
    }

private:
    void buildBg()
    {
        int w = width(), h = height();
        if (w <= 0 || h <= 0) return;

        m_bg = QPixmap(w, h);
        m_bg.fill(Qt::transparent);
        QPainter p(&m_bg);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

        // Rounded clip
        QPainterPath clip;
        clip.addRoundedRect(0, 0, w, h, TV::HERO_R, TV::HERO_R);
        p.setClipPath(clip);

        // Background image or gradient
        if (!m_data.imagePath.isEmpty() && QFile::exists(m_data.imagePath)) {
            QPixmap src(m_data.imagePath);
            src = src.scaled(w, h, Qt::KeepAspectRatioByExpanding,
                             Qt::SmoothTransformation);
            p.drawPixmap(-(src.width()-w)/2, -(src.height()-h)/2, src);
        } else {
            int hue = m_data.title.isEmpty() ? 220
                    : qAbs(m_data.title[0].unicode()*53+190)%360;
            QLinearGradient g(0, 0, w, h);
            g.setColorAt(0, QColor::fromHsv(hue,          130, 58));
            g.setColorAt(1, QColor::fromHsv((hue+50)%360, 100, 22));
            p.fillRect(0, 0, w, h, g);
        }

        // Left-side text scrim: opaque on left, transparent by 55% width
        QLinearGradient scrim(0, 0, w*.58, 0);
        scrim.setColorAt(0.0, QColor(0, 0, 0, 230));
        scrim.setColorAt(0.6, QColor(0, 0, 0, 160));
        scrim.setColorAt(1.0, QColor(0, 0, 0,   0));
        p.fillRect(0, 0, w, h, scrim);

        // Bottom scrim
        QLinearGradient bot(0, h-80, 0, h);
        bot.setColorAt(0, QColor(0,0,0,0));
        bot.setColorAt(1, QColor(0,0,0,120));
        p.fillRect(0, h-80, w, 80, bot);

        p.end();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Multi-layer glow when focused
        if (m_focused) {
            for (int i = 8; i >= 1; --i) {
                QPainterPath gp;
                gp.addRoundedRect(QRectF(i,i,width()-2*i,height()-2*i),
                                  TV::HERO_R+2, TV::HERO_R+2);
                p.setPen(QPen(QColor(168,199,250, 9*i), 2));
                p.setBrush(Qt::NoBrush);
                p.drawPath(gp);
            }
        }

        // Background
        if (!m_bg.isNull()) p.drawPixmap(0, 0, m_bg);

        // ── Text panel ────────────────────────────────────────────────────
        const int pad   = 48;
        const int maxW  = int(width() * 0.46);
        int ty          = height() / 2 - 80;

        // "FEATURED" badge
        p.setFont(QFont("Roboto", 10, QFont::Bold));
        p.setPen(Qt::NoPen);
        p.setBrush(TV::FOCUS_CLR);
        QRect badge(pad, ty, 90, 22);
        p.drawRoundedRect(badge, 4, 4);
        p.setPen(QColor(10,10,10));
        p.drawText(badge, Qt::AlignCenter, "FEATURED");
        ty += 32;

        // Title
        QFont titleFont("Roboto", 32, QFont::Bold);
        p.setFont(titleFont);
        p.setPen(TV::TEXT_PRI);
        QRect titleR(pad, ty, maxW, 80);
        p.drawText(titleR, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                   m_data.title);
        QFontMetrics tfm(titleFont);
        int titleLines = (tfm.boundingRect(titleR, Qt::TextWordWrap,
                                           m_data.title).height() + tfm.height()-1)
                         / tfm.height();
        ty += qMin(titleLines, 2) * tfm.height() + 10;

        // Meta
        p.setFont(QFont("Roboto", 13));
        p.setPen(TV::TEXT_SEC);
        p.drawText(QRect(pad, ty, maxW, 22),
                   Qt::AlignLeft | Qt::AlignVCenter, m_data.meta);
        ty += 30;

        // Description (up to 3 lines)
        if (!m_data.description.isEmpty()) {
            p.setFont(QFont("Roboto", 13));
            p.setPen(QColor(200, 200, 200));
            p.drawText(QRect(pad, ty, maxW, 66),
                       Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                       m_data.description);
            ty += 76;
        }

        // Play hint
        p.setFont(QFont("Roboto", 12, QFont::Medium));
        p.setPen(m_focused ? TV::FOCUS_CLR : QColor(140,140,140));
        p.drawText(QRect(pad, ty, maxW, 22),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   m_focused ? "Press Enter to Play" : "Select to Play");

        // Focus border
        if (m_focused) {
            QPainterPath bp;
            bp.addRoundedRect(QRectF(1.5,1.5,width()-3,height()-3),
                              TV::HERO_R, TV::HERO_R);
            p.setPen(QPen(TV::FOCUS_CLR, 3));
            p.setBrush(Qt::NoBrush);
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

    void keyPressEvent(QKeyEvent *e) override { e->ignore(); }

    void mousePressEvent(QMouseEvent *e) override
    {
        setFocus();
        if (m_confirmCb) m_confirmCb(this);
        QWidget::mousePressEvent(e);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  ContentCard  –  standard 16:9 and square music tiles
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
    void buildThumb()
    {
        int w = width(), h = height();
        m_thumb = QPixmap(w, h);
        m_thumb.fill(Qt::transparent);
        QPainter p(&m_thumb);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

        QPainterPath clip;
        clip.addRoundedRect(0, 0, w, h, TV::CARD_R, TV::CARD_R);
        p.setClipPath(clip);

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

        int scrimH = m_music ? 55 : 82;
        QLinearGradient scrim(0, h-scrimH, 0, h);
        scrim.setColorAt(0, QColor(0,0,0,0));
        scrim.setColorAt(1, QColor(0,0,0, m_music ? 195 : 232));
        p.fillRect(0, h-scrimH, w, scrimH, scrim);

        QFont tf("Roboto", m_music ? 11 : 13, QFont::Bold);
        p.setFont(tf);
        p.setPen(TV::TEXT_PRI);
        QString el = QFontMetrics(tf).elidedText(m_data.title,Qt::ElideRight,w-20);
        p.drawText(QRect(10, h-(m_music?28:52), w-20, 22),
                   Qt::AlignLeft|Qt::AlignVCenter, el);

        if (!m_music) {
            p.setFont(QFont("Roboto", 10));
            p.setPen(TV::TEXT_SEC);
            p.drawText(QRect(10, h-28, w-20, 20),
                       Qt::AlignLeft|Qt::AlignVCenter, m_data.meta);
        }
        p.end();
    }

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

        if (m_focused && m_music) paintPlayOverlay(p);

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

    void keyPressEvent(QKeyEvent *e) override { e->ignore(); }

    void mousePressEvent(QMouseEvent *e) override
    {
        setFocus();
        if (m_confirmCb) m_confirmCb(this);
        QWidget::mousePressEvent(e);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  SidebarOverlay  –  now includes Home at the top
// ─────────────────────────────────────────────────────────────────────────────
class SidebarOverlay : public QWidget
{
    struct NavItem { QString label; Category cat; };
    QVector<NavItem> m_items;
    int              m_sel    = 0;
    Category         m_active = Category::Home;

    std::function<void(Category)> m_selectCb;
    std::function<void()>         m_closeCb;

    enum { ITEM_H=60, ITEM_Y0=210, ITEM_GAP=8,
           ICON_CX=56, ICON_SZ=24, LABEL_X=88 };

public:
    explicit SidebarOverlay(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedWidth(TV::SIDEBAR_W);
        m_items = { {"Home",   Category::Home  },
                    {"Search", Category::Search },
                    {"Movies", Category::Movies },
                    {"Series", Category::Series },
                    {"Music",  Category::Music  } };
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

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(16, 16, 20, 248));
        p.drawRect(rect());

        // Avatar
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

        p.setPen(QColor(255,255,255,20));
        p.drawLine(24, 130, width()-24, 130);

        // Nav items
        for (int i = 0; i < m_items.size(); ++i) {
            bool sel    = (i == m_sel);
            bool active = (m_items[i].cat == m_active);

            QRect pill(18, ITEM_Y0 + i*(ITEM_H+ITEM_GAP),
                       width()-36, ITEM_H);

            p.setPen(Qt::NoPen);
            if (sel) {
                p.setBrush(QColor(238,238,238));
                p.drawRoundedRect(pill, pill.height()/2, pill.height()/2);
            } else if (active) {
                p.setBrush(QColor(168,199,250,35));
                p.drawRoundedRect(pill, pill.height()/2, pill.height()/2);
            }

            QColor ic = sel ? QColor(30,30,30) : TV::TEXT_PRI;
            QRectF iconR(pill.left()+ICON_CX-ICON_SZ/2,
                         pill.center().y()-ICON_SZ/2,
                         ICON_SZ, ICON_SZ);
            switch (m_items[i].cat) {
            case Category::Home:   drawHomeIcon  (p, iconR, ic); break;
            case Category::Search: drawSearchIcon(p, iconR, ic); break;
            case Category::Movies: drawMoviesIcon(p, iconR, ic); break;
            case Category::Series: drawSeriesIcon(p, iconR, ic); break;
            case Category::Music:  drawMusicIcon (p, iconR, ic); break;
            }

            p.setPen(ic);
            p.setFont(QFont("Roboto", 16, sel ? QFont::Bold : QFont::Normal));
            p.drawText(QRect(pill.left()+LABEL_X, pill.top(),
                             pill.width()-LABEL_X-8, pill.height()),
                       Qt::AlignLeft|Qt::AlignVCenter, m_items[i].label);
        }

        // Settings gutter
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
            if (!m_item.filePath.isEmpty() && QFile::exists(m_item.filePath))
                QProcess::startDetached("mpv",
                    QStringList() << "--vo=gpu"
                    << "--audio-device=alsa/sysdefault:CARD=hdmi0"
                    << m_item.filePath);
            else
                qDebug() << "No media file for:" << m_item.title;
            if (m_closeCb) m_closeCb();
        } else {
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

    int cardH() const
    {
        int h = IMGH;
        h += 16 + 38;
        h += 32;
        if (!m_item.description.isEmpty()) h += 92;
        h += (BTNH+10) + BTNH + BPAD;
        return h;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        p.fillRect(rect(), QColor(0,0,0,158));

        const int dh = cardH();
        const int cx = (width()-DW)/2, cy = (height()-dh)/2;
        QRect card(cx, cy, DW, dh);

        for (int i = 10; i >= 1; --i) {
            QPainterPath sp;
            sp.addRoundedRect(card.adjusted(-i,-i,i,i+2), 18, 18);
            p.fillPath(sp, QColor(0,0,0,7));
        }

        QPainterPath cardPath;
        cardPath.addRoundedRect(card, 16, 16);
        p.fillPath(cardPath, TV::SURFACE);

        p.setClipPath(cardPath);
        p.drawPixmap(cx, cy, m_thumb);
        p.setClipping(false);

        QLinearGradient fade(0, cy+IMGH-60, 0, cy+IMGH);
        fade.setColorAt(0, QColor(TV::SURFACE.red(),TV::SURFACE.green(),
                                  TV::SURFACE.blue(), 0));
        fade.setColorAt(1, TV::SURFACE);
        p.fillRect(cx, cy+IMGH-60, DW, 60, fade);

        int ty = cy + IMGH + 16;

        p.setFont(QFont("Roboto", 21, QFont::Bold));
        p.setPen(TV::TEXT_PRI);
        p.drawText(QRect(cx+BPAD, ty, DW-BPAD*2, 32),
                   Qt::AlignLeft|Qt::AlignVCenter, m_item.title);
        ty += 38;

        p.setFont(QFont("Roboto", 13));
        p.setPen(TV::TEXT_SEC);
        p.drawText(QRect(cx+BPAD, ty, DW-BPAD*2, 22),
                   Qt::AlignLeft|Qt::AlignVCenter, m_item.meta);
        ty += 32;

        if (!m_item.description.isEmpty()) {
            p.setFont(QFont("Roboto", 13));
            p.setPen(QColor(185,185,185));
            p.drawText(QRect(cx+BPAD, ty, DW-BPAD*2, 80),
                       Qt::AlignLeft|Qt::AlignTop|Qt::TextWordWrap,
                       m_item.description);
            ty += 92;
        }

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

            if (!primary) {
                p.setPen(QPen(QColor(85,85,96), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(1,1,-1,-1), BTNH/2, BTNH/2);
                p.setPen(Qt::NoPen);
            }

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
    QScrollArea           *hScroll   = nullptr;
    HeroCard              *heroCard  = nullptr;   // non-null only for the hero row
    bool                   isHero    = false;
};

class MainWindow;

class MainWindow : public QWidget
{
    // Start on Home; B button always returns here.
    AppState         m_state    = AppState::Browse;
    Category         m_category = Category::Home;

    QVector<CardRow> m_rows;
    int              m_r = 0, m_c = 0;

    QLabel          *m_header    = nullptr;
    QLabel          *m_infoTitle = nullptr;
    QLabel          *m_infoMeta  = nullptr;
    QScrollArea     *m_vScroll   = nullptr;
    SidebarOverlay  *m_sidebar   = nullptr;
    DetailOverlay   *m_detail    = nullptr;

    QGraphicsOpacityEffect  *m_sidebarOpacity = nullptr;
    QPropertyAnimation      *m_sidebarSlide   = nullptr;
    QPropertyAnimation      *m_sidebarFade    = nullptr;
    QParallelAnimationGroup *m_sidebarAnim    = nullptr;

    bool m_switchingCategory = false;
    bool m_sidebarClosing    = false;

public:
    explicit MainWindow(QWidget *parent = nullptr) : QWidget(parent)
    {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setStyleSheet(QString("QWidget { background-color: %1; }")
                      .arg(TV::BG.name()));

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
        // Launch on Home page
        switchCategory(Category::Home);
    }

    void setInitialFocus()
    {
        if (m_rows.isEmpty()) return;
        // Prefer the hero card when on the home page
        if (m_rows[0].isHero && m_rows[0].heroCard) {
            m_rows[0].heroCard->setFocus();
            m_r = 0; m_c = 0;
        } else if (!m_rows[0].cards.isEmpty()) {
            m_rows[0].cards[0]->setFocus();
            m_r = 0; m_c = 0;
        }
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

    // ── Go to Home (B button / back action in Browse mode) ────────────────
    void goHome()
    {
        if (m_category == Category::Home) return;  // already home – no-op
        switchCategory(Category::Home);
    }

    // ── Central key-dispatch ──────────────────────────────────────────────
    void handleKey(int key)
    {
        switch (m_state) {
        case AppState::Sidebar:
            switch (key) {
            case Qt::Key_Up:                         m_sidebar->moveUp();   break;
            case Qt::Key_Down:                       m_sidebar->moveDown(); break;
            case Qt::Key_Return: case Qt::Key_Enter: m_sidebar->confirm();  break;
            // Left or Back/Escape closes sidebar; does NOT go home from here
            case Qt::Key_Escape: case Qt::Key_Back:
            case Qt::Key_Left:                       closeSidebar();        break;
            default: break;
            }
            break;

        case AppState::Detail:
            switch (key) {
            case Qt::Key_Up:                         m_detail->moveUp();   break;
            case Qt::Key_Down:                       m_detail->moveDown(); break;
            case Qt::Key_Return: case Qt::Key_Enter: m_detail->confirm();  break;
            // Back / B from detail → dismiss overlay only (stay on current page)
            case Qt::Key_Escape: case Qt::Key_Back:  closeDetail();        break;
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
                confirmFocused(); break;
            // Back / Escape / B button → go to Home (never close the app)
            case Qt::Key_Escape: case Qt::Key_Back:
                goHome(); break;
            default: break;
            }
            break;
        }
    }

    void activateFocused() { handleKey(Qt::Key_Return); }

    void navigate(int dr, int dc)
    {
        if (m_rows.isEmpty()) return;

        int nr = qBound(0, m_r+dr, m_rows.size()-1);

        // Hero row occupies "column 0" conceptually; it has no siblings.
        // Moving down from it → jump to first card row.
        // Moving up into it → hero card takes focus regardless of m_c.
        if (m_rows[nr].isHero) {
            m_r = nr; m_c = 0;
            if (m_rows[nr].heroCard) m_rows[nr].heroCard->setFocus();
            return;
        }

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

    void onHeroFocused(HeroCard *hero)
    {
        if (m_infoTitle) m_infoTitle->setText(hero->item().title);
        if (m_infoMeta)  m_infoMeta->setText(hero->item().meta);
        m_r = 0; m_c = 0;
    }

    void onCardConfirmed(ContentCard *card)
    {
        const ContentItem &item = card->item();
        if (item.kind == CardKind::Music) {
            if (!item.filePath.isEmpty() && QFile::exists(item.filePath))
                QProcess::startDetached("mpv",
                    QStringList() << "--vo=gpu"
                    << "--audio-device=alsa/sysdefault:CARD=hdmi0"
                    << item.filePath);
            else
                qDebug() << "No audio file:" << item.title;
        } else {
            openDetail(item);
        }
    }

    void onHeroConfirmed(HeroCard *hero)
    {
        openDetail(hero->item());
    }

private:
    // Confirm whichever widget currently has focus (hero or card).
    void confirmFocused()
    {
        if (m_rows.isEmpty()) return;
        if (m_r >= m_rows.size()) return;

        const CardRow &row = m_rows[m_r];
        if (row.isHero && row.heroCard) {
            onHeroConfirmed(row.heroCard);
        } else if (m_c < row.cards.size()) {
            onCardConfirmed(row.cards[m_c]);
        }
    }

    void restoreFocus()
    {
        if (m_rows.isEmpty()) return;
        m_r = qBound(0, m_r, m_rows.size()-1);

        const CardRow &row = m_rows[m_r];
        if (row.isHero && row.heroCard) {
            row.heroCard->setFocus();
        } else if (!row.cards.isEmpty()) {
            m_c = qBound(0, m_c, row.cards.size()-1);
            row.cards[m_c]->setFocus();
        }
    }

    // ── Shell ─────────────────────────────────────────────────────────────
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

        auto *hint = new QLabel("[Home] menu   [Arr] navigate   [Enter] play   [B] back");
        hint->setStyleSheet("color:#3A3A3A; font:13px 'Roboto';"
                            " background:transparent;");
        hl->addWidget(hint);
        root->addWidget(hdr);
        root->addSpacing(12);

        // Info banner
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

    // ── Category switch with slide-in animation ───────────────────────────
    void switchCategory(Category cat)
    {
        if (m_switchingCategory) return;
        m_switchingCategory = true;

        m_category = cat;
        m_rows.clear();
        m_r = 0; m_c = 0;

        static const char *labels[] = {"HOME","SEARCH","MOVIES","SERIES","MUSIC"};
        m_header->setText(labels[static_cast<int>(cat)]);

        auto *vc = new QWidget; vc->setStyleSheet("background:transparent;");
        auto *vl = new QVBoxLayout(vc);
        vl->setContentsMargins(0,0,0,0); vl->setSpacing(TV::ROW_GAP);

        switch (cat) {
        case Category::Home:   buildHome  (vl); break;
        case Category::Search: buildSearch(vl); break;
        case Category::Movies: buildMovies(vl); break;
        case Category::Series: buildSeries(vl); break;
        case Category::Music:  buildMusic (vl); break;
        }
        vl->addStretch();

        // Fade out old content
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

        // Slide + fade in new content
        auto *fx = new QGraphicsOpacityEffect(vc);
        vc->setGraphicsEffect(fx);
        fx->setOpacity(0.0);
        vc->move(36, 0);

        auto *slide = new QPropertyAnimation(vc, "pos", vc);
        slide->setDuration(230); slide->setEasingCurve(QEasingCurve::OutCubic);
        slide->setStartValue(QPoint(36,0)); slide->setEndValue(QPoint(0,0));

        auto *fade = new QPropertyAnimation(fx, "opacity", vc);
        fade->setDuration(230); fade->setEasingCurve(QEasingCurve::OutCubic);
        fade->setStartValue(0.0); fade->setEndValue(1.0);

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

    // Home page:  Hero banner  +  Frequently Played  +  Recently Watched
    void buildHome(QVBoxLayout *vl)
    {
        using CI = ContentItem;

        // ── Hero (featured item) ──────────────────────────────────────────
        CI featured = {
            CardKind::Movie, "galactic_quest.png", "GALACTIC QUEST",
            "2024 \u00b7 2h 18m \u00b7 Sci-Fi",
            "A rogue crew races across the galaxy to prevent an ancient "
            "weapon from falling into the wrong hands.",
            "/media/big_buck_bunny_1080p_surround.avi"
        };

        auto *heroW = new QWidget; heroW->setStyleSheet("background:transparent;");
        auto *heroL = new QVBoxLayout(heroW);
        heroL->setContentsMargins(0,0,0,0);

        auto *hero = new HeroCard(featured, heroW);
        hero->setFocusCb  ([this](HeroCard *h){ onHeroFocused(h);   });
        hero->setConfirmCb([this](HeroCard *h){ onHeroConfirmed(h); });
        heroL->addWidget(hero);

        vl->addWidget(heroW);

        CardRow heroRow;
        heroRow.isHero   = true;
        heroRow.heroCard = hero;
        m_rows.append(heroRow);

        // ── Frequently Played ─────────────────────────────────────────────
        QVector<CI> frequent = {
            {CardKind::Movie,"galactic_quest.png","GALACTIC QUEST",
             "2024 \u00b7 2h 18m \u00b7 Sci-Fi",
             "A rogue crew races across the galaxy to prevent an ancient "
             "weapon from falling into the wrong hands.",
             "/media/big_buck_bunny_1080p_surround.avi"},
            {CardKind::Movie,"","NOVA RISING",
             "2022 \u00b7 1h 28m \u00b7 Action",
             "An unlikely hero must master ancient powers to stop a rising "
             "empire from plunging the world into chaos.",""},
            {CardKind::Series,"","STELLAR DRIFT",
             "S1 \u00b7 8 Episodes \u00b7 Action",
             "A smuggler and a soldier forge an unlikely alliance against "
             "a collapsing interstellar empire.",""},
            {CardKind::Movie,"","DEEP HORIZON",
             "2024 \u00b7 1h 8m \u00b7 Drama",
             "Beneath three miles of ocean, researchers discover something "
             "that was never meant to be found.",""},
            {CardKind::Series,"","ECHO CHAMBER",
             "S3 \u00b7 12 Episodes \u00b7 Sci-Fi",
             "A whistleblower wakes in a simulation \u2014 and suspects "
             "she\u2019s not the only one trapped inside.",""},
            {CardKind::Movie,"","IRON MERIDIAN",
             "2024 \u00b7 2h 2m \u00b7 Action",
             "An ex-special-forces operative goes rogue to dismantle a "
             "weapons network spanning three continents.",""},
        };
        addRow(vl, "Frequently Played", frequent);

        // ── Recently Watched ──────────────────────────────────────────────
        QVector<CI> recent = {
            {CardKind::Movie,"","JOURNEY TO MARS",
             "2023 \u00b7 1h 54m \u00b7 Drama",
             "An astronaut\u2019s solo mission to Mars forces her to confront "
             "isolation, fear, and what it means to be human.",""},
            {CardKind::Movie,"","THE VOID",
             "2024 \u00b7 52m \u00b7 Documentary",
             "A deep-dive into the mysteries of black holes.",""},
            {CardKind::Series,"","DARK ARCHIVE",
             "S2 \u00b7 10 Episodes \u00b7 Thriller",
             "An archivist stumbles on classified files that rewrite "
             "everything she thought she knew about her city.",""},
            {CardKind::Movie,"","BLUE FREQUENCY",
             "2023 \u00b7 1h 30m \u00b7 Sci-Fi",
             "A radio engineer picks up a signal from 1977 \u2014 and the "
             "voice on the other end knows her name.",""},
        };
        addRow(vl, "Recently Watched", recent);
    }

    void buildSearch(QVBoxLayout *vl)
    {
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

    // ── Build one horizontal card row (no hero) ───────────────────────────
    void addRow(QVBoxLayout *parent, const QString &title,
                const QVector<ContentItem> &items)
    {
        CardRow row;

        auto *rowW = new QWidget; rowW->setStyleSheet("background:transparent;");
        auto *rl   = new QVBoxLayout(rowW);
        rl->setContentsMargins(0,0,0,0); rl->setSpacing(10);

        auto *lbl = new QLabel(title);
        lbl->setStyleSheet(QString(
            "color:%1; font:bold 14px 'Roboto';"
            " letter-spacing:1px; background:transparent;")
            .arg(TV::TEXT_PRI.name()));
        rl->addWidget(lbl);

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
        if (m_sidebar) m_sidebar->setGeometry(0, 0, TV::SIDEBAR_W, height());
        if (m_detail)  m_detail->setGeometry(0, 0, width(), height());
    }

    // MainWindow itself handles no keys directly – everything goes through
    // DpadFilter → handleKey.  We intentionally do NOT call close() anywhere.
    void keyPressEvent(QKeyEvent *e) override
    {
        QWidget::keyPressEvent(e);   // let DpadFilter handle it
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  DpadFilter  –  routes navigation keys through the state machine
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

        // BTN_START / Qt::Key_Home → sidebar toggle
        const bool isStart = (ke->key() == Qt::Key_Home)
                          || (ke->nativeScanCode() == 314u);
        if (isStart) {
            if      (m_win->appState() == AppState::Browse)  m_win->openSidebar();
            else if (m_win->appState() == AppState::Sidebar) m_win->closeSidebar();
            return true;
        }

        switch (ke->key()) {
        case Qt::Key_Left:   case Qt::Key_Right:
        case Qt::Key_Up:     case Qt::Key_Down:
        case Qt::Key_Return: case Qt::Key_Enter:
        // Escape and Back are now "go home", NOT "quit" – handled in handleKey
        case Qt::Key_Escape: case Qt::Key_Back:
            m_win->handleKey(ke->key());
            return true;
        default:
            return false;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  GamepadReader  –  evdev Xbox controller
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
        if (devPath.isEmpty()) {
            qWarning() << "GamepadReader: no device path – controller disabled.";
            return;
        }
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
                case BTN_SOUTH:  m_win->activateFocused();            break; // A → play/confirm
                case BTN_EAST:   m_win->handleKey(Qt::Key_Back);      break; // B → go home
                case BTN_START:
                    if      (m_win->appState() == AppState::Browse)  m_win->openSidebar();
                    else if (m_win->appState() == AppState::Sidebar) m_win->closeSidebar();
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

    DpadFilter kbDpad(&win);
    app.installEventFilter(&kbDpad);

    // Xbox controller auto-detection.
    // Pass an explicit path on the command line to override auto-detection:
    //   ./myqtapp /dev/input/event3
    const QString gamepadPath = (argc > 1)
        ? QString::fromLocal8Bit(argv[1])
        : findXboxEventDevice();

    GamepadReader gamepad(gamepadPath, &win);

    win.showFullScreen();
    win.setInitialFocus();

    return app.exec();
}

#include "myQtApp.moc"
