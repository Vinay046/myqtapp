#include <QApplication>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPixmap>
#include <QPainter>
#include <QDebug>
#include <QMouseEvent>
#include <QFile>
#include <QProcess>

class MovieCard : public QPushButton {
    QString movieTitle;
    QString moviePath;

public:
    MovieCard(const QString &imagePath, const QString &title, const QString &subtitle, const QString &pathToMovie = "", QWidget *parent = nullptr)
        : QPushButton(parent), movieTitle(title), moviePath(pathToMovie) {
        setFixedSize(300, 450);
        setCursor(Qt::PointingHandCursor);
        setStyleSheet("border: none;");

        QPixmap composed(size());
        composed.fill(Qt::transparent);
        QPainter painter(&composed);

        if (!imagePath.isEmpty() && QFile::exists(imagePath)) {
            QPixmap bg(imagePath);
            bg = bg.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            painter.drawPixmap(0, 0, bg);
        } else {
            painter.fillRect(rect(), QColor("#34495e"));  // fallback background
        }

        QRect textRect(0, height() - 80, width(), 80);
        painter.fillRect(textRect, QColor(0, 0, 0, 160));

        painter.setPen(Qt::white);
        QFont font("Arial", 16, QFont::Bold);
        painter.setFont(font);
        painter.drawText(textRect.adjusted(10, 5, -10, -40), Qt::AlignLeft | Qt::AlignTop, title);
        painter.drawText(textRect.adjusted(10, 30, -10, -10), Qt::AlignLeft | Qt::AlignBottom, subtitle);
        painter.end();

        setIcon(QIcon(composed));
        setIconSize(size());
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (!moviePath.isEmpty() && QFile::exists(moviePath)) {
            qDebug() << "Launching movie:" << movieTitle;
            QStringList args;
            args << "-v"
                 << "--vo=gpu"
                 << "--audio-device=alsa/sysdefault:CARD=hdmi0"
                 << moviePath;
            QProcess::startDetached("mpv", args);
        } else {
            qDebug() << "Clicked on movie:" << movieTitle;
        }
        QPushButton::mousePressEvent(event);
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QWidget window;
    window.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    window.setStyleSheet("background-color: #0e2a38;");
    window.setWindowTitle("My File Library");
    window.setFixedSize(700, 500);

    QVBoxLayout *mainLayout = new QVBoxLayout(&window);

    QLabel *title = new QLabel("My File Library");
    QFont titleFont("Arial", 24, QFont::Bold);
    title->setFont(titleFont);
    title->setStyleSheet("color: white;");
    title->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(title);

    QHBoxLayout *cardsLayout = new QHBoxLayout();
    mainLayout->addLayout(cardsLayout);

    // Replace with actual image and movie paths
    MovieCard *card1 = new MovieCard("galactic_quest.png", "GALACTIC QUEST", "PART I", "/media/big_buck_bunny_1080p_surround.avi");
    MovieCard *card2 = new MovieCard("", "JOURNEY TO MARS", "PART II");  // No movie path

    cardsLayout->addStretch();
    cardsLayout->addWidget(card1);
    cardsLayout->addSpacing(20);
    cardsLayout->addWidget(card2);
    cardsLayout->addStretch();

    window.show();
    return app.exec();
}

