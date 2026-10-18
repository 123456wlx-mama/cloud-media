#include "logindialog.h"
#include "ui_logindialog.h"

#include <QMessageBox>
#include <QRegExp>

LoginDialog::LoginDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::LoginDialog)
{
    ui->setupUi(this);
    //设置一下标题栏
    setWindowTitle("登录&注册");
}

// 析构函数
LoginDialog::~LoginDialog()
{
    delete ui;
}

//注册按钮的响应
void LoginDialog::on_pb_login_register_clicked()
{
    //注册信息采集
    QString tel=ui->le_tel_register->text();
    QString password=ui->le_password_register->text();
    QString confirm=ui->le_confirm_register->text();
    QString name=ui->le_name_register->text();

    QString tmpName=name;
    //过滤
    //1：查看是否输入为空--昵称不能为空
    if(tel.isEmpty()||password.isEmpty()||confirm.isEmpty()||name.isEmpty()||tmpName.remove(" ").isEmpty())
    {
        //弹窗
        QMessageBox::about(this,"提示","输入内容，不可为空");
        return ;
    }

    //2：手机号是否合法
    QRegExp exp("^1[356789][0-9]{9}$");
    bool res=exp.exactMatch(tel);
    if(!res)
    {
        QMessageBox::about(this,"提示","手机号非法");
        return;
    }

    //3：密码是否过长
    if(password.size()>20)
    {
        QMessageBox::about(this,"提示","密码输入太长，长度要小于20");
        return;
    }
    //密码确认要一致
    if(confirm!=password)
    {
        QMessageBox::about(this,"提示","两次输入不一致");
        return;
    }
    //4：昵称是否过长，是否有敏感词汇
    if(name.size()>10)
    {
        QMessageBox::about(this,"提示","昵称过长，不呢个超过10");
        return;
    }
    //发信号-----先要去定义信号
    Q_EMIT SIG_registerCommit(tel,password,name);

}

//注册清空的相应
void LoginDialog::on_pb_clear_register_clicked()
{
    //清空就是对有文本的设置为没有文本
    ui->le_confirm_register->setText("");
    ui->le_name_register->setText("");
    ui->le_password_register->setText("");
    ui->le_tel_register->setText("");

}

//登录按钮的相应
void LoginDialog::on_pb_login_clicked()
{
    //登录信息采集
    QString tel=ui->le_tel->text();
    QString password=ui->le_password->text();
    //过滤
    //1：查看是否输入为空
    if(tel.isEmpty()||password.isEmpty())
    {
        //弹窗
        QMessageBox::about(this,"提示","输入内容，不可为空");
        return ;
    }

    //2：手机号是否合法
    QRegExp exp("^1[356789][0-9]{9}$");
    bool res=exp.exactMatch(tel);
    if(!res)
    {
        QMessageBox::about(this,"提示","手机号非法");
        return;
    }

    //3：密码是否过长
    if(password.size()>20)
    {
        QMessageBox::about(this,"提示","密码输入太长，长度要小于20");
        return;
    }

    //发信号-----先要去定义信号
    Q_EMIT SIG_loginCommit(tel,password);
}

//登录清空的响应
void LoginDialog::on_pb_clear_clicked()
{
    ui->le_tel->setText("");
    ui->le_password->setText("");
}