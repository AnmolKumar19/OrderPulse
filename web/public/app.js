const POLL_MS = 700;

const el = {
  bestBid: document.getElementById('bestBid'),
  bestAsk: document.getElementById('bestAsk'),
  spread: document.getElementById('spread'),
  bidsList: document.getElementById('bidsList'),
  asksList: document.getElementById('asksList'),
  ordersList: document.getElementById('ordersList'),
  tradeTape: document.getElementById('tradeTape'),
  orderForm: document.getElementById('orderForm'),
  orderError: document.getElementById('orderError'),
  priceInput: document.getElementById('priceInput'),
  qtyInput: document.getElementById('qtyInput'),
  simulateBtn: document.getElementById('simulateBtn'),
  resetBtn: document.getElementById('resetBtn'),
};

function fmtPrice(cents) {
  return '$' + (cents / 100).toFixed(2);
}

function render(book) {
  el.bestBid.textContent = book.bestBid ? fmtPrice(book.bestBid.price) : '—';
  el.bestAsk.textContent = book.bestAsk ? fmtPrice(book.bestAsk.price) : '—';
  el.spread.textContent = (book.bestBid && book.bestAsk)
    ? fmtPrice(book.bestAsk.price - book.bestBid.price)
    : '—';

  renderLadder(el.bidsList, book.bids, 'bid');
  renderLadder(el.asksList, book.asks, 'ask');
  renderOrders(book.orders);
  renderTape(book.trades);
}

function renderLadder(container, levels, side) {
  if (!levels.length) {
    container.innerHTML = '<div class="empty-note">No resting orders.</div>';
    return;
  }
  const maxQty = Math.max(...levels.map(l => l.qty), 1);
  container.innerHTML = levels.map(l => {
    const pct = Math.round((l.qty / maxQty) * 100);
    return `<div class="ladder-row">
      <div class="depth-bar" style="width:${pct}%"></div>
      <span class="price">${fmtPrice(l.price)}</span>
      <span>${l.qty}</span>
      <span>${l.count}</span>
    </div>`;
  }).join('');
}

function renderOrders(orders) {
  // Only show orders this browser session created (id >= 1,000,000) --
  // simulated flow uses the low id range and would otherwise flood this list.
  const mine = orders.filter(o => o.id >= 1000000);
  if (!mine.length) {
    el.ordersList.innerHTML = '<div class="empty-note">No resting orders yet.</div>';
    return;
  }
  el.ordersList.innerHTML = mine.map(o => `
    <div class="order-row ${o.side}">
      <span>#${o.id} ${o.side.toUpperCase()} ${o.qty} @ ${fmtPrice(o.price)}</span>
      <button class="cancel-btn" data-cancel-id="${o.id}">cancel</button>
    </div>
  `).join('');
  el.ordersList.querySelectorAll('[data-cancel-id]').forEach(btn => {
    btn.addEventListener('click', () => cancelOrder(btn.dataset.cancelId));
  });
}

function renderTape(trades) {
  if (!trades.length) {
    el.tradeTape.innerHTML = '<div class="empty-note">No trades yet.</div>';
    return;
  }
  el.tradeTape.innerHTML = trades.map(t => `
    <div class="tape-row">
      <span>${fmtPrice(t.price)}</span>
      <span class="tape-qty">${t.qty} shares</span>
    </div>
  `).join('');
}

async function fetchBook() {
  const res = await fetch('/api/book');
  render(await res.json());
}

async function cancelOrder(id) {
  const res = await fetch(`/api/orders/${id}`, { method: 'DELETE' });
  if (res.ok) render(await res.json());
}

el.orderForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  el.orderError.textContent = '';
  const side = el.orderForm.querySelector('input[name=side]:checked').value;
  const type = el.orderForm.querySelector('input[name=type]:checked').value;
  const price = parseInt(el.priceInput.value, 10);
  const qty = parseInt(el.qtyInput.value, 10);

  const res = await fetch('/api/orders', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ side, type, price, qty }),
  });
  const data = await res.json();
  if (!res.ok) {
    el.orderError.textContent = data.error || 'order rejected';
    return;
  }
  render(data.book);
});

el.simulateBtn.addEventListener('click', async () => {
  const res = await fetch('/api/simulate', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ events: 200 }),
  });
  if (res.ok) render(await res.json());
});

el.resetBtn.addEventListener('click', async () => {
  const res = await fetch('/api/reset', { method: 'POST' });
  if (res.ok) render(await res.json());
});

fetchBook();
setInterval(fetchBook, POLL_MS);
