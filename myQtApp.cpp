#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMoveEvent>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

class AnimatedButton : public QPushButton
{
public:
    AnimatedButton(const QString &text, QWidget *parent = nullptr)
        : QPushButton(text, parent)
    {
        setMinimumSize(260, 160);
        setFocusPolicy(Qt::StrongFocus);
        applyNormalStyle();
    }

protected:
    void focusInEvent(QFocusEvent *event) override
    {
        QPushButton::focusInEvent(event);
        runFocusInAnimation();
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QPushButton::focusOutEvent(event);
        runFocusOutAnimation();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QPushButton::resizeEvent(event);
        if (!hasFocus()) {
            m_normalGeometry = geometry();
            m_hasStoredGeometry = true;
        }
    }

    void moveEvent(QMoveEvent *event) override
    {
        QPushButton::moveEvent(event);
        if (!hasFocus()) {
            m_normalGeometry = geometry();
            m_hasStoredGeometry = true;
        }
    }

private:
    QRect m_normalGeometry;
    bool m_hasStoredGeometry = false;
    QPropertyAnimation *m_geometryAnim = nullptr;

    void applyNormalStyle()
    {
        setStyleSheet(
            "QPushButton {"
            "  background-color: #2d3748;"
            "  color: white;"
            "  border-radius: 22px;"
            "  border: 3px solid #4a5568;"
            "  font-size: 22px;"
            "  font-weight: 600;"
            "  padding: 18px;"
            "  outline: none;"
            "}"
            "QPushButton:focus {"
            "  outline: none;"
            "}"
            "QPushButton:pressed {"
            "  background-color: #1f2937;"
            "}"
        );
    }

    void applyFocusedStyle()
    {
        setStyleSheet(
            "QPushButton {"
            "  background-color: #374151;"
            "  color: white;"
            "  border-radius: 22px;"
            "  border: 3px solid #63b3ed;"
            "  font-size: 22px;"
            "  font-weight: 600;"
            "  padding: 18px;"
            "  outline: none;"
            "}"
            "QPushButton:focus {"
            "  outline: none;"
            "}"
            "QPushButton:pressed {"
            "  background-color: #1f2937;"
            "}"
        );
    }

    QRect enlargedRect(const QRect &r, int dx, int dy) const
    {
        return QRect(r.x() - dx / 2,
                     r.y() - dy / 2,
                     r.width() + dx,
                     r.height() + dy);
    }

    void stopCurrentAnimation()
    {
        if (m_geometryAnim) {
            m_geometryAnim->stop();
            m_geometryAnim->deleteLater();
            m_geometryAnim = nullptr;
        }
    }

    void animateGeometry(const QRect &start, const QRect &end, int durationMs)
    {
        stopCurrentAnimation();

        m_geometryAnim = new QPropertyAnimation(this, "geometry", this);
        m_geometryAnim->setDuration(durationMs);
        m_geometryAnim->setStartValue(start);
        m_geometryAnim->setEndValue(end);
        m_geometryAnim->setEasingCurve(QEasingCurve::OutCubic);

        connect(m_geometryAnim, &QPropertyAnimation::finished, this, [this]() {
            if (m_geometryAnim) {
                m_geometryAnim->deleteLater();
                m_geometryAnim = nullptr;
            }
        });

        m_geometryAnim->start();
    }

    void runFocusInAnimation()
    {
        if (!m_hasStoredGeometry) {
            m_normalGeometry = geometry();
            m_hasStoredGeometry = true;
        }

        applyFocusedStyle();
        animateGeometry(geometry(), enlargedRect(m_normalGeometry, 24, 16), 140);
    }

    void runFocusOutAnimation()
    {
        applyNormalStyle();

        if (m_hasStoredGeometry) {
            animateGeometry(geometry(), m_normalGeometry, 120);
        }
    }
};

class SectionPage : public QWidget
{
public:
    SectionPage(const QString &titleText, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setStyleSheet("background-color: #111827;");

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(40, 40, 40, 40);
        root->setSpacing(30);

        auto *headerRow = new QHBoxLayout();
        headerRow->setSpacing(20);

        backButton = new AnimatedButton("Home / Back", this);
        backButton->setMinimumSize(220, 100);

        auto *title = new QLabel(titleText, this);
        title->setStyleSheet(
            "QLabel {"
            "  color: white;"
            "  font-size: 34px;"
            "  font-weight: 700;"
            "}"
        );

        headerRow->addWidget(backButton, 0, Qt::AlignLeft);
        headerRow->addWidget(title, 0, Qt::AlignVCenter);
        headerRow->addStretch();

        auto *placeholder = new QLabel("Content for " + titleText, this);
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setStyleSheet(
            "QLabel {"
            "  color: #cbd5e1;"
            "  font-size: 28px;"
            "  border: 2px dashed #475569;"
            "  border-radius: 20px;"
            "  padding: 40px;"
            "}"
        );

        root->addLayout(headerRow);
        root->addStretch();
        root->addWidget(placeholder);
        root->addStretch();
    }

    AnimatedButton *backButton = nullptr;
};

class MediaCenterWindow : public QWidget
{
public:
    MediaCenterWindow(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
        setStyleSheet("background-color: #111827;");
        setCursor(Qt::BlankCursor);

        stack = new QStackedWidget(this);
        stack->setStyleSheet("QStackedWidget { background-color: #111827; }");

        buildHomePage();
        buildSectionPages();

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->addWidget(stack);

        stack->setCurrentWidget(homePage);
        musicPage->hide();
        moviesPage->hide();
        seriesPage->hide();

        musicButton->setFocus();
    }

private:
    QStackedWidget *stack = nullptr;

    QWidget *homePage = nullptr;
    SectionPage *musicPage = nullptr;
    SectionPage *moviesPage = nullptr;
    SectionPage *seriesPage = nullptr;

    AnimatedButton *musicButton = nullptr;
    AnimatedButton *moviesButton = nullptr;
    AnimatedButton *seriesButton = nullptr;

    void buildHomePage()
    {
        homePage = new QWidget(this);
        homePage->setStyleSheet("background-color: #111827;");

        auto *root = new QVBoxLayout(homePage);
        root->setContentsMargins(40, 40, 40, 40);
        root->setSpacing(30);

        auto *title = new QLabel("Media Center", homePage);
        title->setStyleSheet(
            "QLabel {"
            "  color: white;"
            "  font-size: 38px;"
            "  font-weight: 700;"
            "}"
        );

        auto *subTitle = new QLabel("Choose a library", homePage);
        subTitle->setStyleSheet(
            "QLabel {"
            "  color: #94a3b8;"
            "  font-size: 20px;"
            "}"
        );

        auto *grid = new QGridLayout();
        grid->setSpacing(28);

        musicButton = new AnimatedButton("Music", homePage);
        moviesButton = new AnimatedButton("Movies", homePage);
        seriesButton = new AnimatedButton("Series", homePage);

        grid->addWidget(musicButton, 0, 0);
        grid->addWidget(moviesButton, 0, 1);
        grid->addWidget(seriesButton, 0, 2);

        root->addWidget(title);
        root->addWidget(subTitle);
        root->addStretch();
        root->addLayout(grid);
        root->addStretch();

        stack->addWidget(homePage);
    }

    void buildSectionPages()
    {
        musicPage = new SectionPage("Music", this);
        moviesPage = new SectionPage("Movies", this);
        seriesPage = new SectionPage("Series", this);

        stack->addWidget(musicPage);
        stack->addWidget(moviesPage);
        stack->addWidget(seriesPage);

        connect(musicButton, &QPushButton::clicked, this, [this]() {
            animateToPage(homePage, musicPage, true);
        });

        connect(moviesButton, &QPushButton::clicked, this, [this]() {
            animateToPage(homePage, moviesPage, true);
        });

        connect(seriesButton, &QPushButton::clicked, this, [this]() {
            animateToPage(homePage, seriesPage, true);
        });

        connect(musicPage->backButton, &QPushButton::clicked, this, [this]() {
            animateToPage(musicPage, homePage, false);
        });

        connect(moviesPage->backButton, &QPushButton::clicked, this, [this]() {
            animateToPage(moviesPage, homePage, false);
        });

        connect(seriesPage->backButton, &QPushButton::clicked, this, [this]() {
            animateToPage(seriesPage, homePage, false);
        });
    }

    void animateToPage(QWidget *fromPage, QWidget *toPage, bool forward)
    {
        if (!fromPage || !toPage || fromPage == toPage) {
            return;
        }

        const int w = stack->width();
        const int h = stack->height();

        const QPoint center(0, 0);
        const QPoint fromEnd = forward ? QPoint(-w, 0) : QPoint(w, 0);
        const QPoint toStart = forward ? QPoint(w, 0) : QPoint(-w, 0);

        toPage->setParent(stack);
        toPage->setGeometry(0, 0, w, h);
        toPage->move(toStart);
        toPage->show();
        toPage->raise();

        auto *group = new QParallelAnimationGroup(this);

        auto *fromSlide = new QPropertyAnimation(fromPage, "pos");
        fromSlide->setDuration(180);
        fromSlide->setStartValue(center);
        fromSlide->setEndValue(fromEnd);
        fromSlide->setEasingCurve(QEasingCurve::OutCubic);

        auto *toSlide = new QPropertyAnimation(toPage, "pos");
        toSlide->setDuration(180);
        toSlide->setStartValue(toStart);
        toSlide->setEndValue(center);
        toSlide->setEasingCurve(QEasingCurve::OutCubic);

        group->addAnimation(fromSlide);
        group->addAnimation(toSlide);

        connect(group, &QParallelAnimationGroup::finished, this, [=]() {
            stack->setCurrentWidget(toPage);

            fromPage->move(0, 0);
            toPage->move(0, 0);
            fromPage->hide();

            if (toPage == homePage) {
                musicButton->setFocus();
            } else if (toPage == musicPage) {
                musicPage->backButton->setFocus();
            } else if (toPage == moviesPage) {
                moviesPage->backButton->setFocus();
            } else if (toPage == seriesPage) {
                seriesPage->backButton->setFocus();
            }

            group->deleteLater();
        });

        group->start();
    }
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    MediaCenterWindow window;
    window.showFullScreen();

    return app.exec();
}