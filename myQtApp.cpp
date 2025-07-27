#include <QApplication>
#include <QLabel>
#include <QFont>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    QLabel label("Hello Qt6!");
    
    // Remove window decorations (title bar, buttons, etc.)
    label.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    
    // Set font size and alignment
    QFont font = label.font();
    font.setPointSize(48);
    font.setBold(true);
    label.setFont(font);
    label.setAlignment(Qt::AlignCenter);
    
    // Set background color and size
    label.setStyleSheet("QLabel { background-color: #2c3e50; color: #ecf0f1; }");
    label.resize(800, 600);
    
    // Show the window
    label.show();
    
    return app.exec();
}
