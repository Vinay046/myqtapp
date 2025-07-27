#include <QApplication>
#include <QMainWindow>
#include <QPushButton>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QMainWindow window;
    QPushButton *button = new QPushButton("Hello, Qt6 on Wayland!", &window);
    button->setGeometry(10, 10, 300, 100);  // Optional

    window.setCentralWidget(button);
    window.setWindowFlags(Qt::FramelessWindowHint);  // Optional: removes title bar
    window.showFullScreen();

    return app.exec();
}

