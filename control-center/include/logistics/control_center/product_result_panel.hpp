#pragma once

#include <QPixmap>
#include <QUrl>
#include <QWidget>

#include "logistics/control_center/current_product_state.hpp"
#include "logistics/control_center/operations_dashboard_state.hpp"

class QLabel;
class QListWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QResizeEvent;

namespace logistics::control_center {

class ProductResultPanel final : public QWidget {
public:
    explicit ProductResultPanel(QUrl image_base_url, QWidget* parent = nullptr);
    void setCurrentProduct(const CurrentProduct& product);
    void setActiveWorks(const QList<CurrentProduct>& products, const QList<ProcessUnitStatus>& processes);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void setValue(QLabel* label, const QString& value);
    void setImagePlaceholder(const QString& text, bool is_error = false);
    void loadImage(const CurrentProduct& product);
    void updateImagePixmap();
    void showSelectedWork();

    QUrl image_base_url_;
    QNetworkAccessManager* network_manager_{ nullptr };
    QNetworkReply* active_image_reply_{ nullptr };
    QListWidget* active_work_list_{ nullptr };
    QLabel* tracking_status_{ nullptr };
    QLabel* recognition_status_{ nullptr };
    QLabel* processing_status_{ nullptr };
    QLabel* image_label_{ nullptr };
    QLabel* work_id_value_{ nullptr };
    QLabel* barcode_value_{ nullptr };
    QLabel* product_id_value_{ nullptr };
    QLabel* product_name_value_{ nullptr };
    QLabel* destination_value_{ nullptr };
    QLabel* confidence_value_{ nullptr };
    QLabel* updated_at_value_{ nullptr };
    QLabel* detail_value_{ nullptr };
    QString current_work_id_;
    QString current_image_path_;
    QPixmap source_image_;
    QList<CurrentProduct> active_products_;
    QList<ProcessUnitStatus> processes_;
};

}  // namespace logistics::control_center
