WITH user_activity AS (
  SELECT
    u.user_id,
    u.username,
    u.region,
    COUNT(DISTINCT o.order_id) as total_orders,
    SUM(o.quantity) as total_items_purchased,
    MIN(o.order_date) as first_order_date,
    MAX(o.order_date) as last_order_date,
    EXTRACT(EPOCH FROM (MAX(o.order_date) - MIN(o.order_date))) / 86400 as customer_lifetime_days,
    AVG(pm.amount) as avg_order_value
  FROM users u
  LEFT JOIN orders o ON u.user_id = o.user_id
  LEFT JOIN payments pm ON o.order_id = pm.order_id
  GROUP BY u.user_id, u.username, u.region
),
inventory_status AS (
  SELECT
    p.product_id,
    p.product_name,
    p.category,
    p.price,
    w.warehouse_id,
    w.warehouse_name,
    w.region as warehouse_region,
    i.stock_level,
    COUNT(o.order_id) as orders_count,
    SUM(o.quantity) as total_quantity_sold,
    CASE
      WHEN i.stock_level < 100 THEN 'Low'
      WHEN i.stock_level < 500 THEN 'Medium'
      ELSE 'High'
    END as stock_status
  FROM products p
  JOIN inventory i ON p.product_id = i.product_id
  JOIN warehouses w ON i.warehouse_id = w.warehouse_id
  LEFT JOIN orders o ON p.product_id = o.product_id
  GROUP BY p.product_id, p.product_name, p.category, p.price,
           w.warehouse_id, w.warehouse_name, w.region, i.stock_level
),
review_patterns AS (
  SELECT
    r.product_id,
    COUNT(*) as review_count,
    AVG(r.rating) as avg_rating,
    STDDEV(r.rating) as rating_stddev,
    COUNT(CASE WHEN r.rating >= 4 THEN 1 END) as positive_reviews,
    COUNT(CASE WHEN r.rating <= 2 THEN 1 END) as negative_reviews,
    PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY r.rating) as median_rating
  FROM reviews r
  GROUP BY r.product_id
),
monthly_trends AS (
  SELECT
    DATE_TRUNC('month', o.order_date) as order_month,
    p.category,
    u.region,
    COUNT(DISTINCT o.order_id) as monthly_orders,
    SUM(o.quantity * p.price) as monthly_revenue,
    COUNT(DISTINCT o.user_id) as active_customers
  FROM orders o
  JOIN products p ON o.product_id = p.product_id
  JOIN users u ON o.user_id = u.user_id
  GROUP BY DATE_TRUNC('month', o.order_date), p.category, u.region
)
SELECT
  ua.username,
  ua.region,
  ua.total_orders,
  ua.total_items_purchased,
  ua.customer_lifetime_days,
  ua.avg_order_value,
  ist.product_name,
  ist.category,
  ist.warehouse_name,
  ist.warehouse_region,
  ist.stock_level,
  ist.stock_status,
  ist.orders_count,
  ist.total_quantity_sold,
  rp.review_count,
  rp.avg_rating,
  rp.rating_stddev,
  rp.positive_reviews,
  rp.negative_reviews,
  rp.median_rating,
  mt.order_month,
  mt.monthly_orders,
  mt.monthly_revenue,
  mt.active_customers,
  DENSE_RANK() OVER (PARTITION BY ist.warehouse_region ORDER BY ist.stock_level DESC) as stock_rank,
  PERCENT_RANK() OVER (PARTITION BY ua.region ORDER BY ua.avg_order_value) as value_percentile,
  NTILE(4) OVER (ORDER BY ua.customer_lifetime_days) as customer_quartile,
  FIRST_VALUE(ist.stock_level) OVER (PARTITION BY ist.product_id ORDER BY ist.stock_level DESC) as max_stock,
  SUM(mt.monthly_revenue) OVER (PARTITION BY mt.category ORDER BY mt.order_month ROWS BETWEEN 2 PRECEDING AND CURRENT ROW) as rolling_3month_revenue
FROM user_activity ua
JOIN orders o ON ua.user_id = o.user_id
JOIN inventory_status ist ON o.product_id = ist.product_id
LEFT JOIN review_patterns rp ON ist.product_id = rp.product_id
JOIN monthly_trends mt ON ist.category = mt.category AND ua.region = mt.region
WHERE ua.total_orders > 2
  AND ist.stock_level > 0
  AND rp.avg_rating IS NOT NULL
ORDER BY ua.avg_order_value DESC, ist.stock_level DESC
LIMIT 1000;
