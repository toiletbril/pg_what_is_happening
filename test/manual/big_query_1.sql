WITH regional_sales AS (
  SELECT
    u.region,
    p.category,
    DATE_TRUNC('day', o.order_date) as order_day,
    SUM(o.quantity * p.price) as total_sales,
    COUNT(DISTINCT o.user_id) as unique_customers,
    AVG(r.rating) as avg_rating
  FROM orders o
  JOIN users u ON o.user_id = u.user_id
  JOIN products p ON o.product_id = p.product_id
  LEFT JOIN reviews r ON r.product_id = p.product_id AND r.user_id = o.user_id
  GROUP BY u.region, p.category, DATE_TRUNC('day', o.order_date)
),
warehouse_performance AS (
  SELECT
    w.warehouse_id,
    w.warehouse_name,
    w.region as warehouse_region,
    COUNT(DISTINCT s.order_id) as shipped_orders,
    AVG(EXTRACT(EPOCH FROM (s.ship_date - o.order_date))) as avg_ship_delay_seconds
  FROM warehouses w
  JOIN shipping s ON w.warehouse_id = s.warehouse_id
  JOIN orders o ON s.order_id = o.order_id
  GROUP BY w.warehouse_id, w.warehouse_name, w.region
),
product_profitability AS (
  SELECT
    p.product_id,
    p.product_name,
    p.category,
    SUM(pm.amount) as total_revenue,
    COUNT(ret.return_id) as return_count,
    AVG(COALESCE(pr.discount_pct, 0)) as avg_discount
  FROM products p
  JOIN orders o ON p.product_id = o.product_id
  JOIN payments pm ON o.order_id = pm.order_id
  LEFT JOIN returns ret ON o.order_id = ret.order_id
  LEFT JOIN promotions pr ON p.product_id = pr.product_id
    AND o.order_date BETWEEN pr.start_date AND pr.end_date
  GROUP BY p.product_id, p.product_name, p.category
)
SELECT
  rs.region,
  rs.category,
  rs.order_day,
  rs.total_sales,
  rs.unique_customers,
  rs.avg_rating,
  wp.warehouse_name,
  wp.shipped_orders,
  wp.avg_ship_delay_seconds,
  pp.total_revenue,
  pp.return_count,
  pp.avg_discount,
  RANK() OVER (PARTITION BY rs.region ORDER BY rs.total_sales DESC) as sales_rank,
  LAG(rs.total_sales) OVER (PARTITION BY rs.region, rs.category ORDER BY rs.order_day) as prev_day_sales,
  SUM(rs.total_sales) OVER (PARTITION BY rs.region ORDER BY rs.order_day ROWS BETWEEN 6 PRECEDING AND CURRENT ROW) as rolling_7day_sales
FROM regional_sales rs
JOIN warehouse_performance wp ON rs.region = wp.warehouse_region
JOIN product_profitability pp ON rs.category = pp.category
WHERE rs.total_sales > 1000
  AND rs.avg_rating > 2.5
  AND pp.return_count < 100
ORDER BY rs.region, rs.total_sales DESC
LIMIT 1000;
