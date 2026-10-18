#ifndef LOGINDIALOG_H
#define LOGINDIALOG_H

#include <QDialog>

namespace Ui {
class LoginDialog;
}

class LoginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LoginDialog(QWidget *parent = nullptr);
    ~LoginDialog();

signals:
    void SIG_registerCommit(QString tel,QString password,QString name);
    void SIG_loginCommit(QString tel,QString password);



private slots:
    void on_pb_login_register_clicked();

    void on_pb_clear_register_clicked();

    void on_pb_login_clicked();

    void on_pb_clear_clicked();

private:
    Ui::LoginDialog *ui;
};

#endif // LOGINDIALOG_H