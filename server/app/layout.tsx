import type { Metadata } from "next";

import "./globals.css";

export const metadata: Metadata = {
  title: "Wedge",
  description: "Send a message to the device.",
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
